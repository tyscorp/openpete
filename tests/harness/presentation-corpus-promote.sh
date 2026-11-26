#!/usr/bin/env bash
#
# presentation-corpus-promote.sh - re-bless the presentation golden.
#
# Rewrites the tier's blessed.tsv rows for the promoted candidate tiles (verdict
# digest, blessed revision, recomp identity, origin), files those tiles into the
# reference store under their digests, and stages the manifest by name. No PNG
# enters the tree. Never a blind overwrite: it prints each row it adds or
# replaces and, unless --all is given, promotes only the named tiles (the
# adjudicated divergences). A tile whose digest equals its row's keeps its row.
#
# The blessed revision is read from the candidate's capture-record.txt, which
# the gate writes before it captures: the first-parent ancestor of HEAD (500
# deep) whose tree over the build's source roots equals the recorded tree is
# written (the record's head when it matches), and the promote is refused
# otherwise, naming the nearest commit and its first differing path
# (scripts/presentation-blessed.py promote). A capture taken at a revision master
# has since moved past therefore promotes at that revision.
#
# Usage:
#   tests/harness/presentation-corpus-promote.sh <candidate_dir> [tile ...] [--game SLUG]
#   tests/harness/presentation-corpus-promote.sh <candidate_dir> --all
#     tile    a golden basename (e.g. cliff-town-vista__canon__300-vk.png) OR a
#             substring filter; only matching candidate tiles are promoted
#     --all   promote every -vk tile in candidate_dir (full re-bless)
#     --game  game slug; selects the manifest (default: the candidate's
#             capture record's game=, else spyro-1)
#     --exclude=SUBSTR
#             leave out tiles whose basename contains SUBSTR (repeatable), e.g.
#             rows held unblessed under --all
#     --environment-accept=REASON
#             admit a capture environment that departs from the manifest
#             header; the header records REASON
#   PRESENTATION_TIER=dragons selects the dragon-rescue tier's manifest; unset,
#   the capture record's tier= selects it.
#
# A golden-fleet run leaves each unit's promotable candidate in
# build/fleet-stream/<run-id>/<unit>/ (its pulled tiles, verdict report,
# capture-device.txt and capture record); a capture from any fleet host is
# admitted on the Environment surface, so after a run:
#   tests/harness/presentation-corpus-promote.sh build/fleet-stream/<run-id>/<unit> <tile>...
#
# The promote runs inside the capture image, on a candidate captured there:
# the manifest header records the capture environment, and a promote whose
# capture departs from it, or which runs outside the image, is refused
# (scripts/presentation-blessed.py, Environment). Started outside the image,
# the script re-executes itself inside it through
# `scripts/golden-capture-container.sh run` (pg_enter_capture_image
# --no-engine), which passes the machine's hostname as OPENPETE_CAPTURE_HOST;
# the container mounts this tree and the reference store's parent directory,
# so candidate_dir lies under one of them. Exit 64 when no container engine
# is present.
#
# A candidate directory holding forwarded-knobs.txt (a capture run with
# check_presentation_golden.sh --env) is refused: its tiles are an A/B. So is
# one holding outside-image.txt (captured outside the capture image).
#
# After running, review `git -C <openpete> diff --cached` and commit deliberately.
#
# Exit: 0 promoted or nothing to promote; 1 refused; 2 usage; 64 no container
# engine.
set -u
cd "$(dirname "$0")/../.." || exit 2
# shellcheck source=scripts/presentation-game.sh
source scripts/presentation-game.sh

# The candidate's capture record names the game and tier it was captured for;
# they apply when the caller names neither (promote refuses a mismatch).
rec_game="$(sed -n 's/^game=//p' "${1:-}/capture-record.txt" 2>/dev/null | tail -1)"
rec_tier="$(sed -n 's/^tier=//p' "${1:-}/capture-record.txt" 2>/dev/null | tail -1)"
if [[ -n "$rec_game" && " $* " != *" --game "* && " $*" != *" --game="* ]]; then
  set -- "$@" --game "$rec_game"
fi
if [[ -n "$rec_tier" && -z "${PRESENTATION_TIER:-}" ]]; then export PRESENTATION_TIER="$rec_tier"; fi
# A capture that forwarded game knobs (check_presentation_golden.sh --env) is
# an A/B, not the stock configuration a golden row records.
rec_knobs="$(pg_knobs_recorded "${1:-}")"
if [[ -n "$rec_knobs" ]]; then
  echo "[promote] REFUSED: ${1:-} was captured with forwarded knobs $rec_knobs (its $PG_KNOBS_FILE); promote only a stock capture"
  exit 1
fi
# A capture taken outside the capture image (OPENPETE_CAPTURE_OUTSIDE_IMAGE)
# is not under the golden's environment.
if [[ -e "${1:-}/$PG_OUTSIDE_IMAGE_FILE" ]]; then
  echo "[promote] REFUSED: ${1:-} was captured outside the capture image (its $PG_OUTSIDE_IMAGE_FILE); promote only an in-image capture"
  exit 1
fi

if ! pg_in_capture_image; then
  pg_enter_capture_image --no-engine tests/harness/presentation-corpus-promote.sh "$@"
  exit $?
fi

GAME="$(pg_parse_game "$@")" || exit 2
pg_resolve "$GAME" || exit 2
CAND="${1:-}"
shift || true

if [[ -z "$CAND" || ! -d "$CAND" ]]; then
  echo "usage: $0 <candidate_dir> [tile ...|--all]" >&2; exit 2; fi

# --game and its value are consumed above; everything else is a tile filter.
all=0; filters=(); skip=0; opts=()
for a in "$@"; do
  if   (( skip ));                then skip=0
  elif [[ "$a" == "--all" ]];     then all=1
  elif [[ "$a" == --exclude=* ]]; then opts+=(--exclude "${a#--exclude=}")
  elif [[ "$a" == --environment-accept=* ]]; then opts+=(--environment-accept "${a#--environment-accept=}")
  elif [[ "$a" == "--game" ]];    then skip=1
  elif [[ "$a" == --game=* ]];    then :
  else                                 filters+=("$a")
  fi
done
if [[ $all -eq 0 && ${#filters[@]} -eq 0 ]]; then
  echo "usage: $0 <candidate_dir> [tile ...|--all]" >&2; exit 2; fi

ALL_ARG=()
[[ $all -eq 1 ]] && ALL_ARG=(--all)
before="$(git hash-object "$PG_BLESSED" 2>/dev/null)"
python3 scripts/presentation-blessed.py promote "$GAME" --tier "$PG_TIER" \
  "${opts[@]}" "${ALL_ARG[@]}" "$CAND" "${filters[@]}" || exit 1
if [[ "$(git hash-object "$PG_BLESSED")" == "$before" ]]; then exit 0; fi

git add -- "$PG_BLESSED"
echo "[presentation-promote] $GAME: staged $PG_BLESSED - review 'git diff --cached' and commit"
