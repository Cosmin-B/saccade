# Writing in this repository

Documentation should sound like an engineer explaining a contract to another engineer.
Use direct sentences, concrete nouns, and measured claims.

## Describe the result

Write what a component owns, accepts, returns, and refuses. Do not narrate how a draft
was produced or preserve abandoned approaches in product documentation. A design target
must be labeled as a target. A supported feature must have reproducible verification
criteria.

Prefer:

> The device registry copies names into fixed storage and freezes before execution.

Avoid broad claims such as "fast," "lightweight," or "secure" without a measurement or
a named mechanism.

## Keep prose readable

- Use sentence-case headings.
- Keep paragraphs focused on one idea.
- Open with the concrete component, operation, constraint, or measured result.
- Let sentence and paragraph length follow the technical thought. Do not repeat one rhythm across a page.
- Use tables for comparable numbers, not for decoration.
- Define uncommon terms where they first matter.
- Use ASCII punctuation in English prose.
- Split prose semicolons into direct sentences.
- Remove generic essay connectors, roadmap paragraphs, fence-sitting, and recap endings.
- Avoid generated-prose vocabulary when a concrete engineering noun or verb is available.
- State the scope of a result once, beside the result. Repeated disclaimers make the prose defensive.
- Vary contrast wording and remove contrasts that add no technical information.
- Keep complete technical sets intact. A necessary list of operations is not a rhetorical three-part list.
- Do not add empty sections or placeholder text.
- Keep links inside the repository or point to a maintained primary source.

The documentation check rejects unfinished markers, common generated-prose residue,
broken local links, and links that escape the repository. The text check rejects tabs,
trailing whitespace, CRLF, and missing final newlines. A publication pass also checks
prose semicolons, repeated transitions, paragraph rhythm, and technical drift.

## Keep private data out

Do not include captured screens, recognized text, account names, credentials, private build
material, absolute workstation paths, or local model-cache locations. Synthetic fixtures
must make their origin and expected output clear.
