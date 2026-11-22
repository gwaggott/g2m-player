"""Test vectors for the core library, from the training code's exporter and NumPy reference.

The training code isn't public. Set G2M_TRAINING to its directory and run
with its Python environment (it needs PyTorch to build the models):

    G2M_TRAINING=DIR DIR/.venv/bin/python core/test/make_vectors.py [NAME ...]   # default: every model

For each test model, writes to core/test/data/:

- NAME.g2m: a random-weight NoteModel, exported by export.export_model;
- NAME.input.f32: the input audio, raw little-endian float32;
- NAME.outputs.f32: reference.py's outputs per block, float32 (blocks, n_pitches, 3 + the
  MPE heads' classes);
- NAME.events.json: reference.py's events, [frame, "on" / "off", pitch, velocity] with
  frame = the block they were emitted after (blocks = the end-of-stream note-offs), and
  for a note-on of a model with MPE heads also its string probabilities and onset type;
- for `small` also NAME.wav (the input as 24-bit PCM, plus half a block) and
  NAME.mid, reference.py's MIDI file for that WAV, for testing g2m-transcribe.

The heads are scaled and their biases centred on the input (as
the training code's export test does) so the outputs cross the decoder's
thresholds often: random weights would otherwise give few notes. A few
pitches' onsets are made rare, so the late fallback (format version 2)
has notes to start.

`small` and `odd` are written as format version 2 (no MPE heads), so the
older format stays tested; `mpe` is version 3 with both MPE heads, scaled
up so their probabilities vary; `bend` is version 4 with those and the
pitch-offset head, and also gets NAME.mpe.mid, reference.py's MPE output
for the same events (from NAME.input.f32, not the WAV); `proj` is version
5: `mpe`'s model with its gated layers projected (k = 5, none, 11, from its
own activations on the input); `half` is version 6: `bend`'s model
projected the same way, with float16 weights (and its NAME.mpe.mid).
"""

import json
import os
import sys
import wave
from pathlib import Path
from types import SimpleNamespace

if not os.environ.get('G2M_TRAINING'):
    sys.exit('make_vectors.py: set G2M_TRAINING to the training code\'s directory')
TRAINING = Path(os.environ['G2M_TRAINING'])
sys.path.insert(0, str(TRAINING / 'src'))

import numpy as np  # noqa: E402
import torch  # noqa: E402

import reference  # noqa: E402
from export import export_model, pack  # noqa: E402
from model import NoteModel  # noqa: E402

HOP = 64
DATA = Path(__file__).resolve().parent / 'data'

MODELS = {
    # the default model's structure, tiny. Decoder settings chosen so the
    # tests reach every branch: strong_onset low enough that some gated
    # onsets (around 0.5 here) start at once, a 3-frame gate (some onsets
    # expire exactly at it), and a fallback threshold the frame probability
    # (near 0.5 with random weights) reaches
    'small': {
        'spec': {'frontend': [[8, 32, 16], [12, 8, 4]], 'channels': 16, 'kernel_size': 3,
                 'dilations': [1, 2, 4], 'gru': 24},
        'pitch_min': 38, 'n_pitches': 5, 'frames': 1000, 'seed': 0, 'wav': True, 'quiet_onsets': [3],
        'decoder': {'onset_threshold': 0.45, 'frame_threshold': 0.5, 'frame_wait': 30, 'off_frames': 15,
                    'gate_frames': 3, 'strong_onset': 0.52, 'retrigger_threshold': 0.0,
                    'fallback_frames': 2, 'fallback_threshold': 0.53},
    },
    # other shapes: three front-end stages, kernel 2, a dilation that isn't a power of 2;
    # no gate and a re-trigger guard, the decoder's other branches
    'odd': {
        'spec': {'frontend': [[4, 16, 4], [6, 8, 4], [8, 4, 4]], 'channels': 12, 'kernel_size': 2,
                 'dilations': [1, 3], 'gru': 10},
        'pitch_min': 40, 'n_pitches': 7, 'frames': 600, 'seed': 1, 'quiet_onsets': [1, 4, 6],
        'decoder': {'onset_threshold': 0.5, 'frame_threshold': 0.5, 'frame_wait': 20, 'off_frames': 5,
                    'gate_frames': 0, 'strong_onset': 1.0, 'retrigger_threshold': 0.8,
                    'fallback_frames': 3, 'fallback_threshold': 0.51},
    },
}
MODELS['mpe'] = {
    # format version 3 with the string and onset-type heads; the decoder as
    # 'small' (gate, fallback), onset-type thresholds below 0.5 so all three
    # types occur
    'spec': {**MODELS['small']['spec'], 'string_head': True, 'onset_type_head': True},
    'pitch_min': 40, 'n_pitches': 8, 'frames': 2000, 'seed': 2, 'quiet_onsets': [2, 5], 'version': 3,
    'decoder': {**MODELS['small']['decoder'], 'legato_threshold': 0.55, 'slide_threshold': 0.4},
}
MODELS['bend'] = {
    # format version 4: the class heads and the pitch-offset curve head; a
    # bend range and dead band other than the defaults
    'spec': {**MODELS['mpe']['spec'], 'pitch_offset_head': True},
    'pitch_min': 40, 'n_pitches': 8, 'frames': 2000, 'seed': 3, 'quiet_onsets': [2, 5], 'version': 4,
    'decoder': {**MODELS['mpe']['decoder'], 'bend_dead_band': 0.3, 'bend_range': 24},
}
MODELS['proj'] = {
    # format version 5: the gated layers' inputs projected, one layer not
    'spec': MODELS['mpe']['spec'], 'projections': [5, 0, 11],
    'pitch_min': 40, 'n_pitches': 8, 'frames': 2000, 'seed': 4, 'quiet_onsets': [2, 5], 'version': 5,
    'decoder': MODELS['bend']['decoder'],
}
MODELS['half'] = {
    # format version 6: float16 weights, on a projected model with every head
    'spec': MODELS['bend']['spec'], 'projections': [5, 0, 11], 'float16': True,
    'pitch_min': 40, 'n_pitches': 8, 'frames': 2000, 'seed': 5, 'quiet_onsets': [2, 5], 'version': 6,
    'decoder': MODELS['bend']['decoder'],
}
HEAD_SCALE = 5.0
# lowers the onset logits of a model's `quiet_onsets` pitches, so they often
# sound without an onset: notes for the decoder's late fallback to find
QUIET_ONSET = 3.0


def bursts(frames, seed):
    """Noise in bursts of random level and length."""
    rng = np.random.default_rng(seed)
    level = np.repeat(rng.random(frames // 25) ** 2, 25 * HOP)
    return (rng.standard_normal(frames * HOP) * level).astype(np.float32)


def make_model(spec, n_pitches, seed, audio, quiet_onsets=()):
    torch.manual_seed(seed)
    m = NoteModel(HOP, n_pitches, **spec).eval()
    with torch.no_grad():
        m.head.weight.mul_(HEAD_SCALE)
        m.head.bias.sub_(m(torch.from_numpy(audio)[None]).mean(dim=(0, 1)).reshape(-1))
        for p in quiet_onsets:
            m.head.bias[3 * p] -= QUIET_ONSET  # head rows are (pitch, output): onset is 3p
        for head in m.extra_heads.values():
            head.weight.mul_(HEAD_SCALE)
    return m


def projected(m, audio, ks):
    """The model with layer l's input projected on its first ks[l] principal components (0: none)."""
    from project import layer_statistics, project_model
    stats, _ = layer_statistics(m, [SimpleNamespace(audio=audio, frames=len(audio) // HOP)], 'cpu')
    bases = [None if k == 0 else torch.linalg.eigh(cov)[1].flip(1)[:, :k].float() for (_, cov), k in zip(stats, ks)]
    return project_model(m, [mean.float() for mean, _ in stats], bases)


def as_version_2(blob):
    """A version 3 file without MPE heads → the same model as version 2."""
    header, tensors = reference.read_model(blob)
    assert not header['heads']
    v2 = {k: v for k, v in header.items() if k not in ('tensors', 'data_floats', 'data_crc32', 'heads')}
    v2['decoder'] = {k: v for k, v in header['decoder'].items() if k not in ('legato_threshold', 'slide_threshold')}
    return pack({**v2, 'format_version': 2}, tensors)


def write_vectors(name, settings):
    audio = bursts(settings['frames'], settings['seed'])
    m = make_model(settings['spec'], settings['n_pitches'], settings['seed'], audio, settings.get('quiet_onsets', ()))
    if settings.get('projections'):
        m = projected(m, audio, settings['projections'])
    config = SimpleNamespace(sample_rate=48000, hop=HOP, pitch_min=settings['pitch_min'],
                             pitch_max=settings['pitch_min'] + settings['n_pitches'] - 1)
    blob = export_model(m, config, settings['decoder'], 5.208, {'run': f'test vectors: {name}'},
                        float16=settings.get('float16', False))
    if settings.get('version', 2) == 2:
        blob = as_version_2(blob)
    header, _ = reference.read_model(blob)
    assert header['format_version'] == settings.get('version', 2)
    player = reference.Player(*reference.read_model(blob))
    outputs, events = [], []
    ref_events, ref_onsets = [], []  # as reference.transcribe gives them, for the MPE output
    for k in range(settings['frames']):
        new = player.process(audio[k * HOP:(k + 1) * HOP])
        for t, kind, pitch, velocity in new:
            ref_events.append((t, kind, pitch, velocity))
            if kind == 'bend':
                semitones = player.last_bends[pitch]
                ref_onsets.append({'semitones': semitones})
                events.append([k, kind, pitch, velocity, semitones])
                continue
            info = player.last_onsets.get(pitch, {}) if kind == 'on' else {}
            ref_onsets.append(info)
            extra = [[float(p) for p in info['string']], info['onset_type']] if info else []
            if 'bend' in info:
                extra += [info['bend'], info['semitones']]
            events.append([k, kind, pitch, velocity] + extra)
        outputs.append(np.concatenate([np.stack(player.last_outputs, axis=1),
                                       *player.model.head_outputs.values()], axis=1))
    end = player.finish()
    events += [[settings['frames'], kind, pitch, velocity] for _, kind, pitch, velocity in end]
    if settings['spec'].get('pitch_offset_head'):
        mpe = reference.mpe_messages(ref_events + end, ref_onsets + [{} for _ in end], player.decoder.bend_range)
        (DATA / f'{name}.mpe.mid').write_bytes(reference.smf_bytes(mpe))
        print(f'{name}: {sum(e[1] == "bend" for e in events)} bend events, {len(mpe)} MPE messages')

    (DATA / f'{name}.g2m').write_bytes(blob)
    audio.astype('<f4').tofile(DATA / f'{name}.input.f32')
    np.asarray(outputs, '<f4').tofile(DATA / f'{name}.outputs.f32')
    (DATA / f'{name}.events.json').write_text(json.dumps(
        {'blocks': settings['frames'], 'n_pitches': settings['n_pitches'], 'events': events}) + '\n')
    ons = sum(e[1] == 'on' for e in events)
    types = {t: sum(len(e) > 5 and e[5] == t for e in events) for t in reference.ONSET_TYPES}
    if any(types.values()):
        print(f'{name}: onset types {types}')
    print(f'{name}: {m.parameter_count()} parameters, {settings["frames"]} blocks, {ons} note-ons, '
          f'{ons - note_ons_without_fallback(settings, outputs)} more than without the late fallback')
    if settings.get('wav'):
        write_wav_and_midi(name, audio, blob)


def note_ons_without_fallback(settings, outputs):
    decoder = reference.Decoder(settings['n_pitches'], settings['pitch_min'],
                                **{**settings['decoder'], 'fallback_frames': 0})
    return sum(kind == 'on' for o in outputs for kind, *_ in decoder.step(o[:, 0], o[:, 1], o[:, 2]))


def write_wav_and_midi(name, audio, blob):
    """NAME.wav (24-bit, with a part block at the end) and reference.py's MIDI file for it, NAME.mid."""
    audio = np.concatenate([audio, audio[:HOP // 2]])
    pcm = np.clip(np.rint(audio * 2 ** 23), -2 ** 23, 2 ** 23 - 1).astype('<i4')
    path = DATA / f'{name}.wav'
    with wave.open(str(path), 'wb') as f:
        f.setnchannels(1)
        f.setsampwidth(3)
        f.setframerate(48000)
        f.writeframes(pcm.view(np.uint8).reshape(-1, 4)[:, :3].tobytes())
    player = reference.Player(*reference.read_model(blob))
    events = reference.transcribe(player, reference.read_wav(path)[0])
    (DATA / f'{name}.mid').write_bytes(reference.midi_bytes(events))
    print(f'{name}.wav: {sum(e[1] == "on" for e in events)} note-ons in {name}.mid')


def main():
    DATA.mkdir(exist_ok=True)
    for name in sys.argv[1:] or MODELS:
        write_vectors(name, MODELS[name])


if __name__ == '__main__':
    main()
