# Writing in this repository

Documentation should sound like an engineer explaining a contract to another engineer.
Use direct sentences, concrete nouns, and measured claims.

## Describe the result

Write what a component owns, accepts, returns, and refuses. Do not narrate how a draft
was produced or preserve abandoned approaches in product documentation. A design target
must be labeled as a target. A supported feature must point to passing evidence.

Prefer:

> The device registry copies names into fixed storage and freezes before execution.

Avoid broad claims such as "fast," "lightweight," or "secure" without a measurement or
a named mechanism.

## Keep prose readable

- Use sentence-case headings.
- Keep paragraphs focused on one idea.
- Use tables for comparable numbers, not for decoration.
- Define uncommon terms where they first matter.
- Use ASCII punctuation in English prose.
- Do not add empty sections or placeholder text.
- Keep links inside the repository or point to a maintained primary source.

The documentation check rejects unfinished markers, common generated-prose residue,
broken local links, and links that escape the repository. The text check also rejects
tabs, trailing whitespace, CRLF, and missing final newlines.

## Keep private data out

Do not include captured screens, recognized text, account names, credentials, signing
material, absolute workstation paths, or local model-cache locations. Synthetic fixtures
must make their origin and expected output clear.
