# Semantic Merge

Specification of the tree-sitter based semantic conflict resolution in the
3-way merge. Sections 1–9 define the behaviour, which the code on
`feature/tree-sitter-semantic-merge-v3` implements (2026-11-18). Section 10
lists what is still open.

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

One option, `OPT_SEMANTIC_MERGE` (`Settings/SemanticMerge`), off by default:

- **Options > Editor > Compare/Merge**: "Resolve conflicts semantically in the
  Merge Result Pane (experimental, tree-sitter)", below "Use Merge Result
  Pane";
- **Merge > Semantic Merge > Resolve Conflicts Semantically (Experimental)**,
  a check item for the same option.

Requirements (`CMergeDoc::GetSemanticMergeUnavailableReason`; checked by
Auto Merge and the commands alike, and shown when a command runs without them):

- three files are compared and the Merge Result Pane option is on;
- a grammar exists for the file type, and all three panes parse with it.

The option does not depend on the syntax highlighting mode. When highlighting
does not use a tree-sitter parser, the view creates one for the file type
(`CMergeEditView::GetTreeSitterParser`), and enabling semantic merge does not
change highlighting.

## 5. Rules

All entry points (section 6) run the same pipeline:

1. R1 runs once over the whole file.
2. For each conflict in document order, R1 within the conflict, then R2 to R6,
   are tried in this order. The first rule that succeeds resolves the
   conflict, and the next conflict is taken.

The order goes from the narrowest change to the widest. R1 and R2 touch only
the conflict's own lines. R3 to R6 rewrite a whole definition.

Code: `CMergeDoc::DoSemanticMergeConflictsToResult` (whole file) and
`TryResolveConflictSemantically` (one conflict), in `MergeDoc.cpp`. The
decisions are MFC-free functions in `SemanticMergeAnalyzer.{h,cpp}`; the
`CMergeDoc` side gathers the lines and applies the result.

Guards common to all rules:

- **G1 Explicit roles.** Base, mine and theirs come from the merge pane
  mapping. No rule infers the base by majority vote or assumes the middle
  pane.
- **G2 Unique names.** A definition is matched across panes by name, and only
  when the name occurs exactly once in each pane involved.
- **G3 Hands off decided segments.** A segment that is Chosen, Edited or Semantic is
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
  - both copies have the same text and the same enclosing definition (in a
    layout-sensitive language the indentation inside the definition is part
    of the text);
  - each copy is a pending one-sided addition that contains the whole
    definition.
- **Result:** the addition that contains nothing the other one lacks is left
  out (taken from base). The other copy stays.
- **Refuses:**
  - different texts, different enclosing definitions, or neither addition
    being a superset of the other;
  - leaving out a copy that a side put in place of base lines: taking the
    base there would bring the removed lines back. The other side's copy is
    left out instead when it holds nothing more.
- **Code:** `ResolveDuplicateAdditionsInResult`, deciding with
  `ChooseDuplicateAdditionToDrop`.

Within one conflict: when both additions land in the same conflict (base empty
there) and one side's added lines appear whole, in the same order and at the
same indentation inside the other side's, the side with more is taken
(`TryContainedAdditionToResult`, `ChooseContainedAdditionToDrop`). Two
different additions stay a conflict. So does a line that the other side put
inside a block of its own (deeper indentation) or, in languages with a
preprocessor, inside a conditional section it opened (`#if` … `#endif`): there
the same line means something else.

### R2 Token merge

Both sides changed the same lines, but different words of them.

- **Applies when:**
  - each side's change to the base is computed token by token (LCS over
    tokens; a string literal and a line comment are one token each);
  - the two sides' changes touch different tokens.
- **Result:** base with both sides' token changes applied. A side that only
  re-wrapped or re-indented keeps its layout without conflicting: the white
  space in front of a kept token is merged three ways as well, so a line
  after the other side's change stays at the indentation it was given.
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

Code: `TryTokenMergeConflictToResult`, deciding with `MergeTokensThreeWay`.

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
- **Code:** `TryMovedDefinitionMergeToResult`, deciding with
  `DetectMovedDefinition` and `MergeMovedDefinition`.

### R4 One-sided definition change

The conflict's lines belong to one definition that only one side changed. The
line conflict comes from neighbouring changes that ended up in the same
difference block.

- **Applies when:** the definition (matched by name) is identical in base and
  in one side, and differs in the other side.
- **Result:** the changed side's definition text.
- **Refuses:**
  - the definition being changed on both sides (that is R2 or R5 territory),
    or the definition being missing from a pane;
  - a changed side that also changed the definition's first or last line
    outside the definition's range (a grammar can start the range after a
    keyword such as `export`). The resolution accounts for whole lines, so
    that change would be lost. R5 refuses the same.

### R5 Small change onto a refactored definition

One side refactored a definition heavily, which defeats R2's volume cap. The
other side made exactly one small change.

- **Applies when** the small side's change against the base is exactly one of
  the following, and nothing else (undoing it gives the base text back):
  - one string literal changed, and the old literal occurs exactly once in
    the refactored version;
  - one parameter renamed together with exactly one use site, and the old
    name occurs exactly twice in the refactored version (C-like languages);
  - one identifier renamed with 2–4 occurrences, and the same count of the
    old name in the refactored version.
- **Result:** the refactored definition with that one change replayed.
- **Refuses:** any second difference on the small side, a replay target that
  is not unique, or a rename whose new name the refactored version already
  uses (two names would become one).

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

R4–R6 are decided by `SemanticMerge::Analyzer::TryBuildSuggestion`, which
returns the pane to rewrite (`anchorPane`) and its new definition text, and
applied by `CMergeDoc::ApplySemanticSuggestionToResult`.

### Not rules

These were in earlier versions of the branch and were removed, because the
line merge already handles them or because they are not merges:

- Inserting a one-sided addition at its definition neighbour's position: a
  one-sided addition is not a conflict.
- Copying a whole file's independent additions into the base pane: the same.
- Adopting the version two panes agree on ("catch-up/revert"): that is a
  plain Copy, not a merge.

## 6. Entry points

| Entry point | Does | Code |
|---|---|---|
| Auto Merge (toolbar, `Merge > Auto Merge`) with the option on | The pipeline over all conflicts, then the non-conflicting differences are merged as before. | `ApplyAutoMergeToResult` |
| **Resolve Conflict Semantically** (`Ctrl+Alt+Shift+M`), from the result pane or a compared pane | R2–R6 on the conflict at the cursor (from a compared pane, the definition under the cursor is preferred). If no rule applies, a message names each rule and why it declined. | `ResolveConflictSemantically` |
| **Resolve All Conflicts Semantically** (`Ctrl+Alt+Shift+A`) | The pipeline over all pending conflicts, without the line merge. | `DoSemanticMergeConflictsToResult` |
| Command line `-semantic` together with `/al`, `/am` or `/ar` | Turns semantic merge on for this run only (a flag on the app, never written to the saved option), then behaves like Auto Merge. With `-o` and `-noninteractive` the result is saved. | `CMergeApp::GetSemanticMergeForRun` |

The commands are in **Merge > Semantic Merge** and in the result pane's
context menu. Without a merge session they start one, as Auto Merge does.
They are disabled while the option or the Merge Result Pane is off. There is
no preview dialog: the result pane shows the change, and Undo reverts it.

Classic Auto Merge (Merge Result Pane off) does not run semantic merge.

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
- **Report.** After Auto Merge, and after Resolve All Conflicts Semantically, a message
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

Baseline (2026-11-18, 395 files from 9 repositories):

- `tokens`: 832 conflicts, 165 merged, 129 confirmed by the committed file.
- `run`, line merge alone: 16 files match the commit, 5 differ, 374 keep
  conflicts; 781 conflicts left.
- `run` with `-semantic`: 46 match, 15 differ, 334 keep conflicts; 640
  conflicts left.

The manual triples, middle = base: `SemanticDuplicate3Way*` (R1),
`SemanticMoved3Way*` (R3), `SemanticStringLiteral3Way*`,
`SemanticParameter3Way*`, `SemanticIdentifier3Way*` (R5),
`SemanticCommented3Way*` (R6), `SemanticDuplicateReplaced3Way*` (R1 where one
side put its copy in place of a base definition: that copy stays and the old
definition does not come back), and three that no rule may touch:
`SemanticAddition3Way*` (two different additions at one place: stays a
conflict), `SemanticSameName3Way*` (the same method added to two different
classes: R1 must keep both) and `SemanticWrapped3Way*` (one side added a call,
the other the same call inside an `if`: stays a conflict).

## 10. Status

Done on 2026-11-18, the ten work items of the first version of this spec:

1. Visibility (section 8).
2. One pipeline for every entry point. Classic Auto Merge (no Merge Result
   Pane) no longer runs semantic merge.
3. Explicit roles in the analyzer (`SemanticMerge::MergeRoles`).
4. G4 in every rule (`IsSemanticResultSyntaxOk` for R1 and R3–R6; R2 checks
   its own lines).
5. Result pane only. Removed: the direct pane edits, the preview dialog, the
   To Left/Middle/Right commands, and the insertion, independent-addition and
   adopt-agreed suggestions.
6. One option, `OPT_SEMANTIC_MERGE`, independent of syntax highlighting.
7. `-semantic` replaces `-sml`/`-smm`/`-smr`.
8. `Ctrl+Alt+Shift+A` is bound to Resolve All Conflicts Semantically.
9. R1 and R3 decisions are MFC-free and unit-tested; the manual triples use
   the middle = base layout.
10. Manual text for the option, `/semantic`, the rules and the shortcuts, in
    `Docs/Manual` (the `winmerge-manual` submodule).

Found on the way: R5's string literal check compared only the literals, so a
side with other code changes plus one changed literal passed, and the other
changes were lost. The check now requires that undoing the literal change
gives the base text back.

A later review found six more ways to a wrong merge, each reproduced with a
small triple and now refused or merged correctly:

- R2 and R3: a line after one side's change lost the indentation the other
  side gave it (Python: it fell out of its block).
- R5: a rename replayed onto a version that already used the new name.
- R4 and R5: a change on the definition's first line in front of its range
  (`export`) was dropped.
- R1 in a conflict: a line that the other side wrapped in a block counted as
  the same addition.
- R1: leaving out a copy that replaced base lines brought those lines back.
- R1: in Python, two definitions that differ only in indentation counted as
  the same.

Open:

- The new UI strings are not in `Translations/WinMerge/English.pot`, like the
  branch's other strings.
- The margin tooltip and Next/Previous Semantic Resolution are not checked in
  a running app: MFC shows tooltips only in the active window.
- The `Docs/Manual` changes need a pull request to `winmerge-manual`.

Not part of this feature. These are on the branch and go to separate pull
requests:

- tree-sitter caret breadcrumb in the status bar (`OPT_TREE_SITTER_BREADCRUMB`);
- symbol occurrence highlighting (`OPT_TREE_SITTER_OCCURRENCE_HIGHLIGHT`);
- `/checkonly`;
- Ignore Trailing Punctuation (`OPT_CMP_IGNORE_TRAILING_PUNCTUATION`,
  `/ignoretrailingpunctuation`);
- `StructuralDiff` (no caller; see `StructuralDiffPlan.md`);
- the date-literal DST fix in filter expressions.
