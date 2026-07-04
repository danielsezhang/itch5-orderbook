#!/usr/bin/env bash
# Downloads the Nasdaq BX ITCH 5.0 full-day sample used by the historical test and benchmarks.
set -euo pipefail

name=20191230.BX_ITCH_50
url="https://emi.nasdaq.com/ITCH/Nasdaq%20BX%20ITCH/${name}.gz"
expected_gz_bytes=390561039
dir="$(cd "$(dirname "$0")/.." && pwd)/data"
mkdir -p "$dir"

if [[ -f "$dir/$name" ]]; then
  echo "already present: $dir/$name"
  exit 0
fi

curl --fail --location --continue-at - -A "Mozilla/5.0" -o "$dir/$name.gz" "$url"
actual=$(wc -c < "$dir/$name.gz" | tr -d ' ')
if [[ "$actual" != "$expected_gz_bytes" ]]; then
  echo "size mismatch: got $actual bytes, expected $expected_gz_bytes" >&2
  exit 1
fi
gunzip --keep "$dir/$name.gz"
echo "wrote $dir/$name"
