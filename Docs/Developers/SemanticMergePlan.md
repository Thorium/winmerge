# Semantic Merge Plan

## Implementation Status (2026-06-12)

Phase 1 plumbing is in place and productization has started:

- The analysis core lives in `Src/SemanticMergeAnalyzer.{h,cpp}` (`SemanticMerge::Analyzer`),
  a pure, MFC-free layer: panes are described by an `ITextSource` plus tree-sitter
  `TagRange` lists, differences by plain line ranges. `CMergeDoc` only adapts buffers,
  runs precondition checks (option enabled, 3-way, writable pane) and applies
  suggestion records through the normal `CDiffTextBuffer` edit path.
- `CTreeSitterParser` no longer depends on MFC or `CCrystalTextBuffer`; it consumes the
  `ITreeSitterTextAccess` interface (`Externals/crystaledit/editlib/TreeSitterTextAccess.h`),
  which `CCrystalTextBuffer` implements. This made the UnitTests and FolderCompare
  projects buildable again.
- Unit tests: `Testing/GoogleTest/SemanticMerge/SemanticMergeAnalyzer_test.cpp` covers the
  localized-change helpers and all analyzer scenarios, partly over the manual triples in
  `Testing/Data/SemanticMergeManual/`.
- UI commands ("Safe Semantic Copy" menu), the experimental option
  (`OPT_SEMANTIC_MERGE_EXPERIMENTAL`, off by default) and the CLI flags
  (`-sml`/`-smm`/`-smr`, force-enable in-memory for the run) are wired.

Bugs found by the tests and fixed:

- The commented-out-function update path was unreachable: it required a same-named tag
  in the destination pane, but a fully commented-out function has no tag there. Reworked
  to match tags only in the two non-destination panes and to target the diff's line range
  in the destination; covered by tests and the `SemanticCommented3Way*` manual triple.
- The identifier-rename detection required the occurrence delta to be 1 and could never
  fire for a real rename; fixed to require a clean name swap with equal occurrence
  counts (2-4 occurrences).

Language handling:

- The text-based heuristics are gated per language via `SemanticMerge::LanguageTraits`
  (`TraitsForLanguage`): line-comment prefixes, string quote characters, C-style
  parameter lists, identifier renames. Unknown and markup languages get all heuristics
  disabled; whole-definition replace and insertion suggestions stay language-agnostic.
  Replacing the textual scanners with tree-sitter node queries remains future work.

## Goal

Investigate an experimental semantic merge feature for text files using tree-sitter data, with the feature exposed as an addition to the existing line-based merge workflow rather than a replacement.

The main target is to improve cases where line-based 3-way merge produces avoidable conflicts, especially when:

- one side reorders top-level definitions and the other edits one of those definitions
- both sides add different top-level definitions near the same line numbers
- line-based alignment interleaves blocks poorly even when the edits are semantically independent
- one side performs a larger refactor while the other side makes a small, localized semantic edit within the same function
- one side updates a function while the other side comments out that same function, and the commented-out copy should still receive the latest content

## Current State In This Repository

The repository already contains the key prerequisites for an experimental semantic merge layer:

- existing 3-way merge and auto-merge logic is line-range based
- tree-sitter parsing is already integrated per pane
- tree-sitter tags and locals queries are already being collected for navigation and highlighting support

Relevant code areas:

- `Src/MergeDocDiffCopy.cpp`: current `DoAutoMerge()` implementation
- `Src/DiffList.*`: diff classification and mergeability decisions
- `Src/MergeDoc.cpp`: parser lifecycle per buffer
- `Externals/crystaledit/editlib/TreeSitterParser.*`: tree-sitter parsing, tags, locals, byte-to-line mapping

## Product Direction

This feature should be introduced as experimental and additive.

Preferred product behavior:

- keep the current merge engine as the default behavior
- add an experimental option to enable semantic merge assistance
- surface semantic resolution through new commands or suggestions, not as a silent replacement of existing auto-merge

This keeps risk low and makes it easier to evaluate correctness before expanding scope.

## UI Strategy

Two UI mechanisms make sense and can coexist.

### 1. Experimental Toggle

Add a new option such as:

- `Enable experimental safe semantic copy suggestions`

Behavior:

- off by default
- only active in text compare/merge modes where tree-sitter is available for all participating panes
- if disabled, current behavior remains unchanged

Why this is useful:

- clearly communicates that the feature is exploratory
- avoids changing established merge behavior unexpectedly
- lets testers opt in globally

### 2. New Commands

Add explicit commands such as:

- `Suggest semantic merge for current conflict`
- `Suggest semantic merges`
- `Apply safe semantic copy`

Behavior:

- commands are enabled only when the current document is 3-way text compare and semantic analysis is available
- command output should be reviewable before applying edits

Why this is useful:

- users can invoke the feature only when needed
- easier to trust than silent auto-resolution
- fits the expectation that semantic merging is not yet mature enough to replace existing merge operations

### Recommended First Release

Ship both:

- a global experimental option controlling availability
- explicit commands for generating and applying suggestions

This is safer than wiring the feature into `Auto Merge` directly.

## Scope For A First Version

The first version should be intentionally narrow.

Recommended supported cases:

- top-level function or method reorder on one side plus body edit on the other side
- independent top-level additions on both sides
- whole-definition moves where the definition body is unchanged on one side
- whole-definition edit on one side and unchanged placement on the other side
- one side performs a larger refactor of a function while the other side changes only a function parameter and the matching in-function usage sites
- one side performs a larger refactor of a function while the other side changes only a string literal or user-visible text inside that function
- one side comments out a whole function or method while the other side updates that same function or method, with the desired result being an updated commented-out version

Recommended out of scope initially:

- statement-level semantic merge inside function bodies
- deep class-member reordering across languages with weak tags support
- macro-heavy or preprocessor-heavy C and C++ files
- languages without usable `tags.scm` coverage
- automatic resolution when both sides edit the same definition body

## Technical Approach

### 1. Expose Semantic Units From Tree-Sitter

Add read-only APIs to `CTreeSitterParser` to expose top-level semantic units.

A semantic unit should contain at least:

- symbol kind
- symbol name
- optional qualified name
- start and end byte range
- start and end line range
- source text for the range, or a stable content hash

The initial implementation should be based on existing tags-query results where possible.

### 2. Build A 3-Way Semantic Index

Create a new analyzer, for example `SemanticMergeAnalyzer`, owned or invoked by `CMergeDoc`.

Input:

- base, left, and right buffers
- corresponding `CTreeSitterParser` instances

Responsibilities:

- extract top-level units from each pane
- match units across base, left, and right
- classify each unit as unchanged, moved, edited, added, deleted, or ambiguous

Matching priority should be conservative:

- qualified name + kind
- name + kind
- only use content similarity as a fallback heuristic

### 3. Generate Suggestions, Not Direct Merges

The analyzer should produce suggestion records rather than mutating buffers directly.

Each suggestion should include:

- reason the merge looks safe
- source semantic units involved
- target line range in the destination pane
- proposed result text
- confidence level

Example safe case:

- left: function `f` moved only
- right: function `f` body changed only
- result: keep left ordering, take right body

Additional target cases:

- left: function `f` heavily refactored but still matches the same semantic unit, right: parameter name or parameter value changed with one corresponding use site updated, result: replay the small semantic edit onto the refactored version when the affected parameter and use site can be mapped confidently
- left: function `f` heavily refactored, right: only a string literal changed, result: replay the string literal change onto the refactored version when the literal can be matched to the same statement or call site confidently
- left: function `f` updated normally, right: function `f` commented out, result: keep the function commented out but replace its commented body text with the latest function content when the commented-out block can be matched confidently to the same base function

### 4. Apply Suggestions Through Existing Edit Paths

Application should reuse normal edit/copy operations where possible so that:

- undo remains correct
- buffer bookkeeping stays consistent
- rescan behavior remains predictable

The semantic layer should not bypass normal text-buffer editing rules.

## Integration Points

Likely integration points in the current codebase:

- `CMergeDoc` as the coordinator for semantic analysis
- `CTreeSitterParser` for exposing top-level definitions/tags
- merge UI command handlers in `MergeEditView.cpp`
- options plumbing for the experimental toggle

The current `DoAutoMerge()` logic should not be changed first. The initial feature should live beside it.

## Suggestion Presentation

The user experience should make it obvious that the result is advisory.

Possible presentation approaches:

- add a command that selects the current conflict and opens a preview dialog
- add a non-modal suggestion pane for semantic resolutions
- add a context-menu action on a conflict block when a semantic suggestion exists

For the first version, the simplest workable UX is likely:

- command generates suggestion for the current conflict
- confirmation dialog previews the replacement text
- user explicitly applies or cancels

## Safety Rules

The feature should refuse to suggest or apply semantic merge when:

- tree-sitter is unavailable for one or more panes
- semantic unit matching is ambiguous
- both left and right changed the same semantic unit body
- extracted ranges overlap inconsistently
- the language grammar provides poor or missing tag coverage
- a larger refactor changes control flow or duplicates candidate match sites so a small edit cannot be mapped to a single confident destination

Conservative behavior is more important than wide coverage.

## Small-Edit-On-Refactor Heuristic

The two added classic cases need a narrower heuristic than top-level move detection.

Target pattern:

- one side contains a broader refactor of a matched function or method
- the other side contains a very small edit inside that same matched function or method

Promising first heuristics:

- parameter change plus a single related use-site change
- one string literal change
- one identifier rename with one or very few matching references in the same function

Suggested implementation strategy:

- first match the enclosing top-level semantic unit across base, left, and right
- compute a small intra-function edit summary for the side with the localized change
- use tree-sitter nodes and surrounding syntax context to locate the corresponding node in the refactored version
- only produce a suggestion when exactly one confident target exists

Examples of confidence signals:

- same enclosing function matched across all three panes
- same statement kind and similar surrounding call structure
- same argument position or same parameter slot
- unique string literal or unique identifier occurrence within the matched function

Examples of rejection conditions:

- multiple candidate string literals after refactor
- parameter usage duplicated, deleted, or moved into multiple branches
- refactor rewrites the function so heavily that old and new subtrees no longer map reliably

This heuristic should remain suggestion-only in the experimental phase.

## Commented-Out Function Update Heuristic

This case is useful but differs from the others because a fully commented-out function may no longer exist as a normal syntax node in the tree-sitter parse.

Target pattern:

- base contains function `f`
- one side updates function `f`
- the other side comments out function `f`
- desired result is a commented-out copy of the updated function, not the stale commented-out body

Suggested implementation strategy:

- match the normal function node from base to the updated side using normal semantic-unit matching
- detect that the other side replaced the function range with a comment block whose text strongly resembles the base function text
- strip comment markers from the commented-out block and compare the recovered text to the base function text
- if the match is strong and unique, render the updated function text back into the same comment style

Possible supported comment styles:

- line comments applied to each line
- block comments wrapping the whole function

Confidence signals:

- the commented block appears at or near the original function location
- decommented text closely matches the base function body or signature
- only one plausible commented-out match exists for that function

Rejection conditions:

- multiple nearby commented blocks could match the same function
- mixed comment formatting makes decommenting unreliable
- the commented block contains manual notes or edits that are not part of the original function text
- there is no strong textual match back to the base function

This should be implemented as a hybrid semantic-plus-text suggestion rather than a pure AST merge.

## Relation To Existing Moved-Block Logic

WinMerge already has moved-block detection, but it is line-equivalence based rather than symbol-aware.

Semantic merge should complement, not replace, this logic.

Practical division of responsibilities:

- moved-block detection continues to help visualize and align line movement
- semantic merge suggestion layer handles symbol-aware cases that line movement alone cannot resolve safely

## Research Expectations

This is a relatively open design area from a product-integration standpoint.

Reasonable expectation:

- there is useful prior art in AST-aware diffing and structural merge tools
- there is not likely to be a single widely accepted standard workflow that maps directly onto WinMerge's current architecture and UI

So the implementation should be driven primarily by:

- the specific failure modes WinMerge users want to improve
- conservative safety criteria
- incremental rollout with opt-in feedback

## Recommended Phases

### Phase 1: Design And Plumbing

- add the experimental option
- add disabled-by-default UI commands
- expose top-level semantic units from `CTreeSitterParser`
- add a non-mutating analyzer that can classify candidate safe merges

Deliverable:

- diagnostics and suggestion generation only

### Phase 2: Previewable Suggestions

- generate suggestions for current conflict or whole file
- show preview and explanation
- allow explicit apply

Deliverable:

- user-reviewed semantic merge suggestions for a narrow set of cases

### Phase 3: Broader Coverage

- improve matching heuristics
- add more languages with verified tags support
- consider optional integration with existing auto-merge workflow after confidence is established

Deliverable:

- broader but still opt-in semantic assistance

## Recommended First Implementation Decision

Implement this as:

- an experimental global option enabling semantic merge assistance
- new commands for suggestion generation and application
- no behavior change to existing `Auto Merge`

This is the lowest-risk path and best matches the current uncertainty around feature maturity.

## Open Questions

- which options page should host the experimental toggle
- whether suggestions should be generated per conflict, per file, or both
- whether preview should be a dialog or integrated pane
- which languages should be enabled first
- how much explanation to show when a suggestion is rejected as ambiguous
