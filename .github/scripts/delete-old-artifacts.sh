#!/usr/bin/env bash
# Deletes the repository's build artifacts older than DAYS (default 30),
# keeping those built from a commit that a release's tag points to.
# Needs gh, logged in or with GH_TOKEN (actions: write). DRY_RUN=1 only lists.
#
#   .github/scripts/delete-old-artifacts.sh [owner/repo]
set -euo pipefail

repo=${1:-${GITHUB_REPOSITORY:?give owner/repo}}
days=${DAYS:-30}
cutoff=$(date -u -d "$days days ago" +%Y-%m-%dT%H:%M:%SZ)

# The commits that releases (drafts included) are tagged on.
keep=$(gh api --paginate "repos/$repo/releases" --jq '.[].tag_name' |
    while read -r tag; do
        gh api "repos/$repo/commits/$tag" --jq .sha 2>/dev/null || true
    done | sort -u)
echo "Release commits kept: $(echo ${keep:-none})"

deleted=0; kept=0
while IFS=$'\t' read -r id name created sha; do
    [ -n "$id" ] || continue
    if [[ "$created" > "$cutoff" ]]; then continue; fi
    if grep -qx "$sha" <<<"$keep"; then
        echo "keep   $name ($created, ${sha:0:7}: a release)"; kept=$((kept + 1)); continue
    fi
    echo "delete $name ($created, ${sha:0:7})"
    [ "${DRY_RUN:-0}" = 1 ] || gh api -X DELETE "repos/$repo/actions/artifacts/$id"
    deleted=$((deleted + 1))
done < <(gh api --paginate "repos/$repo/actions/artifacts?per_page=100" \
    --jq '.artifacts[] | select(.expired | not) | [.id, .name, .created_at, .workflow_run.head_sha] | @tsv')

[ "${DRY_RUN:-0}" = 1 ] && note=' (dry run: nothing deleted)' || note=''
echo "Older than $days days ($cutoff): $deleted deleted, $kept kept for releases$note."
