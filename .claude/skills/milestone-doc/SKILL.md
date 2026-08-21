---
name: milestone-doc
description: Scaffold the paired docs/design/milestone-N-<slug>.md and docs/milestones/milestone-N.md files for a new printerctl milestone, following this repo's existing frontmatter and section conventions. Use when starting a new milestone or asked to create/update its design or status doc.
---

# milestone-doc

Every printerctl milestone gets two documents, cross-linked to each
other and to `Agents.md`/`architecture.md`/`roadmap.md`:

- `docs/design/milestone-N-<slug>.md` — the design decisions, written
  before or during implementation.
- `docs/milestones/milestone-N.md` — the status/report doc, updated as
  the milestone progresses (summary, files changed, verification,
  risks, decisions, next steps).

This skill scaffolds both from the templates in this skill's
`templates/` directory, matching the structure already used by every
milestone in `docs/` (checked against milestones 1, 6, 9, 11 and 12 —
the frontmatter and the status doc's six sections are consistent across
all of them; the design doc's body sections are not, see below).

## Inputs

Take from `$ARGUMENTS`, or ask if missing:

- **N**: the milestone number (e.g. `13`).
- **TITLE**: a short human title (e.g. "Job Queue Model").
- **SLUG**: kebab-case slug for the design doc filename (e.g.
  `job-queue-model`) — derive it from TITLE if not given.
- **TAGS**: a few lowercase topic tags for the frontmatter (e.g.
  `queue, daemon`) — infer 2-4 from what the milestone is about; do not
  just copy another milestone's tags.

Before creating anything, check whether `docs/milestones/milestone-N.md`
or a `docs/design/milestone-N-*.md` already exists — if so, this is an
update, not a new scaffold: read the existing file and edit it in place
instead of overwriting it.

## Steps

1. Read [templates/milestone-status.md](templates/milestone-status.md)
   and [templates/milestone-design.md](templates/milestone-design.md).
2. Substitute `{{N}}`, `{{TITLE}}`, `{{DATE}}` (today, `YYYY-MM-DD`),
   `{{TAGS}}` and, in the design template, `{{SLUG}}`.
3. Write the design doc to
   `docs/design/milestone-{{N}}-{{SLUG}}.md`.
4. Write the status doc to `docs/milestones/milestone-{{N}}.md`.
5. In the status doc, replace the `## 1. Technical summary` HTML
   comment with real content if enough is already known about the
   milestone's scope (e.g. from a roadmap entry or the user's request);
   otherwise leave the placeholder comments for the milestone's actual
   implementation work to fill in later.
6. In the design doc, replace the placeholder `## 1. ...` heading with
   the milestone's first real topic — do NOT invent a fixed multi-section
   skeleton here. Past design docs cover completely different ground
   (module boundaries, safety policy, state model, API shape,
   whatever this milestone actually needs to decide); the only fixed
   points are the frontmatter, the opening cross-link paragraph, and a
   closing `## N. Deferred` section.
7. Do not touch `status: in_progress` to `status: done`, and do not fill
   in `## Verification` with real numbers — those only become true once
   the milestone is actually implemented and run through the `verify`
   skill.

## Notes

- `roadmap.md` and `architecture.md` are the actual source of truth for
  what a milestone should contain — read the relevant roadmap entry
  before writing the technical summary, don't guess.
- Keep the `[[wikilink|path]]` cross-references exactly in the format
  the templates use; this repo's docs are read as an Obsidian-style
  vault (see the `Fuente de verdad` / `Source of truth` lines already in
  `Components.md`, `context-ia.md` and every milestone doc).
