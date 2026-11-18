# Semantic Merge

Specification of the tree-sitter based semantic conflict resolution in the
3-way merge. Sections 1–9 define the intended behaviour. Section 10 lists
where the code on `feature/tree-sitter-semantic-merge-v3` differs from it
(state of 2026-11-18); those items are the remaining work.

## 1. Purpose

A line-based 3-way merge reports a conflict whenever both sides changed
overlapping lines. Many of those conflicts are not conflicts in terms of what
each side did to the code: one side moved a function and the other edited it,
both sides edited different words of the same line, both sides added the same
helper. Semantic merge resolves such conflicts using the syntax tree of the
file, and leaves every other conflict to the user.

It never replaces the line merge. It runs after the line merge, only on what
the line merge left unresolved (plus one known line-merge mistake, rule R1),
and every resolution it makes is visible, explained and undoable.

## 2. Terms

| Term | Meaning |
|---|---|
| Base, mine, theirs | The three panes as given by the merge pane mapping (`GetMergePaneMapping(m_nMergeBasePane)`). The base is the common ancestor. It is not necessarily the middle pane. |
| Result | The Merge Result pane's buffer. Semantic merge writes only here. |
| Conflict | A pending result segment whose difference is `OP_DIFF` (both sides changed it). |
| Definition | A range reported by the language's tags query (`<language>-tags.scm`): function, method, class, member and so on, with a name. |
| Rule | One resolution strategy (section 5). |
| Resolution | A conflict replaced with text produced by a rule, recorded with the rule and a reason. |

## 3. Scope

In scope:

- 3-way text comparisons with the Merge Result pane (the default,
  `Settings/MergeResultPaneEnabled`).
- Files for which a tree-sitter grammar is available. Rule R2 needs only the
  grammar. All other rules also need a tags query (section 7).

Out of scope, and refused:

- 2-way comparisons, folder compare, and merging without the result pane.
- Writing into the compared panes. They stay as loaded.
- Conflicts where both sides changed the same tokens differently.
- Two different additions at the same place. Replay of real merges showed
  committers keep both in only about 60% of cases, so neither order is safe.
- Regions containing preprocessor lines in C, C++ and C# (`#if`, `#define`, ...).
- Statement reordering inside a function body.

## 4. Enabling

One option: **Resolve conflicts semantically (experimental)**
(`Settings/SemanticMerge`), on the Merge Result options page, off by default.

Requirements, checked per file and shown when unmet:

- three files are compared and the result pane is active;
- a grammar exists for the file type, and all three panes parse with it.

The option does not depend on the syntax highlighting mode. Turning
highlighting off does not turn semantic merge off, and enabling semantic merge
does not change highlighting.

## 5. Rules

All entry points (section 6) run the same pipeline:

1. R1 runs once over the whole file.
2. For each conflict in document order, R2 to R6 are tried in this order. The
   first rule that succeeds resolves the conflict, and the next conflict is
   taken.

The order goes from the narrowest change to the widest. R2 touches only the
conflict's own lines. R3 to R6 rewrite a whole definition.

Guards common to all rules:

- **G1 Explicit roles.** Base, mine and theirs come from the merge pane
  mapping. No rule infers the base by majority vote or assumes the middle
  pane.
- **G2 Unique names.** A definition is matched across panes by name, and only
  when the name occurs exactly once in each pane involved.
- **G3 Hands off decided segments.** A segment that is Chosen or Edited is
  never rewritten, and neither is one whose common text around it was edited
  (`IsResultCommonTextIntact`).
- **G4 Syntax check.** The file with the resolution applied must not have more
  tree-sitter syntax errors than the worse of the two sides' versions of the
  same region.
- **G5 Text, not layout.** Comparisons ignore white space and blank lines
  unless the language is layout-sensitive (Python, F#).

### R1 Duplicate addition

Both sides added the same definition, each in its own place. The line merge
sees two unrelated additions and takes both, which leaves the definition in the
result twice.

- **Applies when:**
  - the definition is absent from base;
  - it appears exactly once in mine and once in theirs;
  - both copies have the same text and the same enclosing definition;
  - each copy is a pending one-sided addition that contains the whole
    definition.
- **Result:** the addition that contains nothing the other one lacks is left
  out (taken from base). The other copy stays.
- **Refuses:** different texts, different enclosing definitions, or neither
  addition being a superset of the other.

### R2 Token merge

Both sides changed the same lines, but different words of them.

- **Applies when:**
  - each side's change to the base is computed token by token (LCS over
    tokens; a string literal and a line comment are one token each);
  - the two sides' changes touch different tokens.
- **Result:** base with both sides' token changes applied. A side that only
  re-wrapped or re-indented keeps its layout without conflicting.
- **Refuses:**
  - empty base (two additions at one place);
  - overlapping changes;
  - a side that changed more tokens than the base region has (minimum 4); see
    the volume cap below;
  - the same word inserted by both sides in different places (it would be
    doubled);
  - both sides adding lines in a layout-sensitive language;
  - preprocessor lines;
  - inputs too large to align (over 60,000 tokens, or a product of the two
    lengths over 6,000,000).

Volume cap: beyond that size the alignment is no longer reliable. Replay
showed this cap is what keeps out a renamed call combined with a re-typed
argument.

### R3 Moved definition

One side moved a definition and the other side edited it where it was. The
line merge sees a removal against an edit (a conflict) plus an unrelated
addition elsewhere.

- **Applies when:**
  - the conflict lies in a base definition;
  - in one side (the mover) that definition no longer overlaps its base
    position, while in the other (the editor) it still does;
  - the editor changed nothing else in the differences involved.
- **Result:** the mover's layout, with the definition at its new place carrying
  the editor's change. If the mover also changed the definition, both changes
  are combined with R2's token merge.
- **Refuses:**
  - a definition that shares lines with another definition;
  - an editor that only reformatted (the line merge already handles a pure
    move);
  - token merge failing.

### R4 One-sided definition change

The conflict's lines belong to one definition that only one side changed. The
line conflict comes from neighbouring changes that ended up in the same
difference block.

- **Applies when:** the definition (matched by name) is identical in base and
  in one side, and differs in the other side.
- **Result:** the changed side's definition text.
- **Refuses:** the definition being changed on both sides (that is R2 or R5
  territory), or the definition being missing from a pane.

### R5 Small change onto a refactored definition

One side refactored a definition heavily, which defeats R2's volume cap. The
other side made exactly one small change.

- **Applies when** the small side's change against the base is exactly one of:
  - one string literal changed, and the old literal occurs exactly once in
    the refactored version;
  - one parameter renamed together with exactly one use site, and the old
    name occurs exactly twice in the refactored version (C-like languages);
  - one identifier renamed with 2–4 occurrences, and the same count of the
    old name in the refactored version.
- **Result:** the refactored definition with that one change replayed.
- **Refuses:** any second difference on the small side, or a replay target
  that is not unique.

### R6 Commented-out definition update

One side commented a definition out line by line. The other side edited it.

- **Applies when:**
  - the conflict in the commenting side is a block where every non-blank line
    starts with the same line-comment prefix;
  - with the prefix removed, the block equals the base definition;
  - the editing side changed that definition.
- **Result:** the edited definition, commented out with the same prefix.
- **Refuses:** a block that does not match the base definition after
  uncommenting, mixed prefixes, or block comments.

### Removed from the pipeline

These exist in the current code (section 10). They are not part of the
feature because the line merge already handles them, or because they are not
merges:

- Inserting a one-sided addition at its definition neighbour's position: a
  one-sided addition is not a conflict.
- Copying a whole file's independent additions into the base pane: the same.
- Adopting the version two panes agree on ("catch-up/revert"): that is a
  plain Copy, not a merge.

## 6. Entry points

| Entry point | Does |
|---|---|
| Auto Merge (toolbar, `Merge > Auto Merge`) with the option on | Line merge, then the pipeline over all remaining conflicts. |
| **Resolve Conflict Semantically** (`Ctrl+Alt+Shift+M`), from the result pane or a compared pane | The pipeline on the current conflict only. If no rule applies, a message names each rule and why it declined. |
| **Resolve All Conflicts Semantically** (`Ctrl+Alt+Shift+A`) | The pipeline over all pending conflicts, without re-running the line merge. |
| Command line `-semantic` together with `/al`, `/am` or `/ar` | Enables the option for this run only, then behaves like Auto Merge. With `-o` and `-noninteractive` the result is saved. |

Commands are disabled, with the reason in the tooltip, when section 4's
requirements are unmet. There is no preview dialog: the result pane shows the
change, and Undo reverts it.

## 7. Languages

Rule R2 works with any grammar. R1 and R3–R6 need a tags query. These ship
today: c, cpp, c_sharp, go, java, javascript (with flow and tsx), typescript,
php, python, ruby, rust, fsharp.

Language traits (`SemanticMerge::TraitsForLanguage`) tune the rules:

| Languages | Line comment | String quotes | C parameters (R5) | Identifier rename (R5) | Preprocessor (R2) | Layout-sensitive |
|---|---|---|---|---|---|---|
| c, cpp, c_sharp | `//` | `"` | yes | yes | yes | no |
| java | `//` | `"` | yes | yes | no | no |
| javascript, typescript, tsx, flow | `//` | `"` `'` | yes | yes | no | no |
| php | `//` `#` | `"` `'` | yes | yes | no | no |
| python | `#` | `"` `'` | no | yes | no | yes |
| ruby, bash | `#` | `"` `'` | no | yes | no | no |
| go, rust | `//` | `"` | no | yes | no | no |
| fsharp | `//` | `"` | no | yes | no | yes |
| others | none | none | no | no | no | no |

A language with no traits gets R1, R2 (structure only), R3 and R4.

## 8. Visibility

Every resolution is recorded and shown:

- **Segment state.** A new `ResultSegmentState::Semantic`, plus the rule
  (`R1`–`R6`) and a one-line reason, for example "Moved definition
  `LoadConfig`: theirs moved it, mine edited it". A resolution is not
  `Edited`. `Edited` means a hand edit.
- **Margin and tooltip.** Semantic segments show `s` in the result margin and
  use the moved-block colours. Hovering the margin shows the rule and the
  reason.
- **Navigation.** Next/Previous Semantic Resolution in the result pane's
  context menu. They step over whole resolutions; one resolution can cover
  several differences.
- **Report.** After Auto Merge, and after Copy All into the result, a message
  shows "Resolved semantically: N" broken down by rule. Each resolution is
  also logged to the Output pane with its result line and reason.
- **Status bar.** The result pane's status bar adds "Semantic: N" to the
  conflict counts. With the caret in a semantic segment, the line information
  shows its rule.
- **Undo and override.** One Undo step per run (Auto Merge) or per command
  restores the conflict. Choose Mine/Base/Theirs on a semantic segment
  replaces it like any other segment.
- **Saving.** A semantic segment saves as its text, with no conflict markers.

## 9. Verification

Each rule must have:

- unit tests on the analysis layer (`Testing/GoogleTest/SemanticMerge`), MFC
  free;
- a manual triple in `Testing/Data/SemanticMergeManual/` named
  `Semantic<Case>3Way{Left,Middle,Right}.<ext>` (middle = base), for the
  command-line check:
  ```
  WinMergeU.exe -noprefs -noninteractive -semantic -am -o out.cpp L.cpp M.cpp R.cpp
  ```
- coverage in the replay corpus (`Testing/SemanticMergeReplay/Replay.ps1`).

Acceptance for a change to any rule:

- `Replay.ps1 tokens`: merged and confirmed counts do not drop.
- `Replay.ps1 run`: no case moves from match to differs, and every new
  "differs" is read by hand.

Baseline (2026-11-18, after merging master):

- `tokens`: 832 conflicts, 165 merged, 129 confirmed by the committed file.
- SQLProvider subset: semantic match 6, differs 4, 67 conflicts left; the line
  merge alone leaves 94.

## 10. Current code against this spec

Work items, in the order they should be done.

1. **Visibility (section 8).** Done 2026-11-18: the `Semantic` state, rule,
   reason and resolution id on `MergeResultSegment`; marking through
   `BeginSemanticResolution`/`EndSemanticResolution` in every rule; margin,
   colours, tooltip, status bar, report, Output pane log and navigation; one
   undo step per Auto Merge. The new UI strings are not yet in
   `Translations/WinMerge/English.pot`, like the branch's other strings.
2. **One pipeline (section 5).**
   - The interactive command and Copy All run only the analyzer (R4–R6 plus
     the removed suggestions), never R1–R3.
   - Classic Auto Merge, without the result pane, runs only the analyzer.
   - Make `DoSemanticMergeConflictsToResult` the single implementation, with
     a per-conflict variant for the command.
3. **Explicit roles (G1).**
   - `Analyzer::FinalizeStandardSuggestion` picks the unchanged pane by
     majority vote.
   - `TryBuildInsertionSuggestion` and `TryCollectIndependentAdditionSuggestions`
     assume the base is pane 1.
   - Pass base, mine and theirs into the analyzer.
4. **Syntax check everywhere (G4).** Only R2 and R3 call `CountSyntaxErrors`.
   Apply the check to R4–R6 in `ApplySemanticSuggestionToResult`.
5. **Result pane only (section 3).** Remove:
   - the direct pane edits (`ReplaceDefinitionText`, `InsertDefinitionText`);
   - the preview dialog (`SemanticPreviewDlg`, `IDD_SEMANTIC_PREVIEW_DLG`);
   - the To Left/Middle/Right commands;
   - the insertion, independent-addition and adopt-agreed suggestions.
6. **One switch (section 4).**
   - Merge `OPT_SEMANTIC_MERGE_EXPERIMENTAL` and
     `OPT_AUTO_MERGE_SEMANTIC_FALLBACK` into one option.
   - `IsTreeSitterEnabled` requires `OPT_SYNTAX_HIGHLIGHT_MODE != 0`, and
     parsers are owned by the views: semantic merge needs its own parsers when
     highlighting is off.
   - Move the option from the Editor page to the Merge Result page.
7. **Command line (section 6).**
   - Replace `-sml`/`-smm`/`-smr` with `-semantic`.
   - Stop forcing `OPT_SYNTAX_HIGHLIGHT_MODE` to 2.
8. **Shortcut.** The menu shows `Ctrl+Alt+Shift+A` for "All to Current Pane",
   but the accelerator table has no entry for it.
9. **Tests.**
   - R1 and R3 live in `CMergeDoc` and are tested only by replay. Move their
     decisions into the MFC-free layer and unit-test them.
   - Commit the untracked triples (`SemanticDuplicate3Way*`,
     `SemanticMoved3Way*`, `SemanticSameName3Way*`) and `Replay.ps1`.
10. **Manual.** Document the option and `-semantic` in the `winmerge-manual`
    repository. The earlier text for the removed flags is in
    `SemanticMerge-manual-Command_line.patch`.

Not part of this feature. These are on the branch and go to separate pull
requests:

- tree-sitter caret breadcrumb in the status bar (`OPT_TREE_SITTER_BREADCRUMB`);
- symbol occurrence highlighting (`OPT_TREE_SITTER_OCCURRENCE_HIGHLIGHT`);
- `/checkonly`;
- Ignore Trailing Punctuation (`OPT_CMP_IGNORE_TRAILING_PUNCTUATION`,
  `/ignoretrailingpunctuation`);
- `StructuralDiff` (no caller; see `StructuralDiffPlan.md`);
- the date-literal DST fix in filter expressions.
