#!/bin/bash
# Start guitar-2-midi on a headless Raspberry Pi: JACK on the audio
# interface, mod-host with the player and a synth, and the connections
#
#   guitar (dry input) → player → synth → interface outputs
#
# Usage: start-g2m.sh [stop]
# Settings below (or in the environment); docs/raspberry-pi.md has the setup.
set -u

JACK_CARD=${JACK_CARD:-hw:Stomp}            # aplay -l
JACK_PERIOD=${JACK_PERIOD:-256}
JACK_PERIODS=${JACK_PERIODS:-3}
DRY_INPUT=${DRY_INPUT:-system:capture_5}     # the HX Stomp's dry guitar signal
OUTPUTS=${OUTPUTS:-"system:playback_1 system:playback_2"}
PLAYER=https://github.com/gwaggott/guitar-2-midi#player
SYNTH=${SYNTH:-http://synthv1.sourceforge.net/lv2}
SYNTH_OUTPUTS=${SYNTH_OUTPUTS:-"Out_L Out_R"}
SYNTH_MIDI=${SYNTH_MIDI:-in}
INPUT_GAIN=${INPUT_GAIN:-12}                 # dB: the HX Stomp's dry signal is quiet (docs/raspberry-pi.md)
PORT=5555
LOGS=${LOGS:-$HOME/g2m-logs}

if [ "${1:-}" = stop ]; then
    pkill -x mod-host
    pkill -x jackd
    exit 0
fi
mkdir -p "$LOGS"

# mod-host's commands: a null-terminated string each, answered "resp N" (N < 0 an error)
mh() {
    python3 - "$@" <<'EOF'
import socket, sys
s = socket.create_connection(('127.0.0.1', 5555), timeout=60)
ok = True
for cmd in sys.argv[1:]:
    s.sendall(cmd.encode() + b'\0')
    data = b''
    while not data.endswith(b'\0'):
        data += s.recv(4096)
    reply = data[:-1].decode()
    ok = ok and not reply.startswith('resp -')
    print(f'{cmd} -> {reply}')
sys.exit(0 if ok else 1)
EOF
}

wait_for() {  # wait_for SECONDS COMMAND...: until COMMAND succeeds
    local seconds=$1; shift
    for _ in $(seq $((seconds * 2))); do "$@" > /dev/null 2>&1 && return 0; sleep 0.5; done
    echo "timed out waiting for: $*" >&2
    return 1
}

if ! pgrep -x jackd > /dev/null; then
    setsid jackd -P 70 -d alsa -d "$JACK_CARD" -r 48000 -p "$JACK_PERIOD" -n "$JACK_PERIODS" \
        > "$LOGS/jackd.log" 2>&1 < /dev/null &
fi
wait_for 15 jack_lsp || exit 1

if ! pgrep -x mod-host > /dev/null; then
    # synthv1 starts Qt, which needs a platform even without a display; no
    # feedback port (-f): mod-host would wait for a client on it
    QT_QPA_PLATFORM=offscreen setsid mod-host -p "$PORT" > "$LOGS/mod-host.log" 2>&1 < /dev/null &
fi
wait_for 15 bash -c "ss -ltn | grep -q ':$PORT '" || exit 1

mh "remove -1" > /dev/null  # a clean slate if mod-host was already running
mh "add $PLAYER 0" "add $SYNTH 1" || exit 1
wait_for 15 jack_lsp effect_1:"${SYNTH_MIDI}" || exit 1
outputs=($OUTPUTS)
synth_outputs=($SYNTH_OUTPUTS)
cmds=("param_set 0 input_gain $INPUT_GAIN"
      "connect $DRY_INPUT effect_0:lv2_in" "connect effect_0:lv2_events_out effect_1:$SYNTH_MIDI")
for i in "${!synth_outputs[@]}"; do
    cmds+=("connect effect_1:${synth_outputs[$i]} ${outputs[$i]}")
done
mh "${cmds[@]}" || exit 1
echo "guitar-2-midi running: $DRY_INPUT → player → $SYNTH → $OUTPUTS (logs in $LOGS)"
