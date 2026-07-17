#!/bin/sh

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <compile_commands.json>" >&2
    exit 2
fi

database=$1
source_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

if [ ! -f "$database" ]; then
    echo "compile database does not exist: $database" >&2
    exit 2
fi
if ! command -v jq >/dev/null 2>&1; then
    echo "jq is required to read the compile database" >&2
    exit 2
fi

entries=$(jq -c --arg root "$source_dir" '
    [.[] |
        select(.file | startswith($root + "/")) |
        select(.file | contains("/tests/") | not) |
        select(.file | test("/(apps|backends|platform|src|tools)/"))] |
    unique_by(.file) |
    .[]
' "$database")
count=$(printf '%s\n' "$entries" | jq -s 'length')

if [ "$count" -eq 0 ]; then
    echo "compile database contains no production translation units" >&2
    exit 2
fi

index=0
printf '%s\n' "$entries" | while IFS= read -r entry; do
    index=$((index + 1))
    directory=$(printf '%s' "$entry" | jq -r '.directory')
    command=$(printf '%s' "$entry" | jq -r '.command')
    file=$(printf '%s' "$entry" | jq -r '.file')
    analyze=$(printf '%s\n' "$command" | sed -E \
        's@ -o [^ ]+ -c @ --analyze -Xanalyzer -analyzer-output=text -Xanalyzer -analyzer-werror -o /dev/null @')

    if [ "$analyze" = "$command" ]; then
        echo "unsupported compile command: $file" >&2
        exit 2
    fi

    printf '[%d/%d] %s\n' "$index" "$count" "$file"
    (cd "$directory" && eval "$analyze")
done

printf 'Analyzed %d production translation units.\n' "$count"
