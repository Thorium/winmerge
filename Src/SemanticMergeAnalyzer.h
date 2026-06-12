/////////////////////////////////////////////////////////////////////////////
//    WinMerge:  an interactive diff/merge utility
//    SPDX-License-Identifier: GPL-2.0-or-later
/////////////////////////////////////////////////////////////////////////////
/**
 * @file  SemanticMergeAnalyzer.h
 *
 * @brief Tree-sitter based semantic merge suggestion analyzer.
 *
 * Pure analysis layer for the experimental semantic merge feature.
 * It has no dependency on CMergeDoc, MFC or the UI: panes are described
 * by an ITextSource plus a list of tree-sitter tag ranges, differences by
 * plain line ranges. The analyzer produces suggestion records; applying
 * them to buffers is the caller's responsibility.
 */
#pragma once

#include "UnicodeString.h"
#include "DiffList.h"
#include "TreeSitterParser.h"
#include <vector>

namespace SemanticMerge
{

using TagRange = CTreeSitterParser::TagRange;

/**
 * @brief Read-only text access for one pane.
 *
 * CMergeDoc adapts CDiffTextBuffer to this; tests can use a plain
 * line-vector implementation.
 */
class ITextSource
{
public:
	virtual int GetLineCount() const = 0;
	virtual int GetLineLength(int nLine) const = 0;
	/**
	 * @brief Get text in the given range, with EOLs normalized the same way
	 * the pane text is rendered (ghost lines excluded for diff buffers).
	 */
	virtual void GetTextRange(int nStartLine, int nStartChar, int nEndLine, int nEndChar, String& text) const = 0;
	virtual String GetLineEol(int nLine) const = 0;

protected:
	~ITextSource() = default;
};

/**
 * @brief Language-dependent knobs for the localized-change heuristics.
 *
 * The text-based heuristics (string literal replay, parameter rename replay,
 * identifier rename replay, commented-block update) are only valid for
 * languages whose syntax they understand. Defaults disable all of them;
 * whole-definition replace and insertion suggestions are language-agnostic
 * and always available.
 */
struct LanguageTraits
{
	std::vector<String> lineCommentPrefixes; /**< empty: commented-block update disabled */
	String stringQuoteChars;                 /**< empty: string literal replay disabled */
	bool cStyleParameters = false;           /**< enables parameter rename replay */
	bool identifierRename = false;           /**< enables identifier rename replay */
};

/**
 * @brief Traits for a tree-sitter language name (e.g. "cpp", "python").
 * Unknown languages get all heuristics disabled.
 */
LanguageTraits TraitsForLanguage(const String& languageName);

/** @brief Analyzer input for one pane. */
struct PaneData
{
	const ITextSource* pText = nullptr;
	std::vector<TagRange> tags;
	bool hasLanguage = false; /**< true when tree-sitter parsed this pane */
};

/** @brief The subset of DIFFRANGE the analyzer needs. */
struct DiffInfo
{
	int dbegin = 0;
	int dend = 0;
	OP_TYPE op = OP_NONE;
	/**
	 * Optional anchor: the pane and line the user invoked the command from.
	 * When the diff spans several definitions, the definition under the
	 * anchor is preferred. -1 means no anchor.
	 */
	int anchorPane = -1;
	int anchorLine = -1;
	/**
	 * Allow the destination pane that holds the single changed version to
	 * adopt the definition the two other panes agree on (catch-up/revert).
	 * Only interactive, previewed commands enable this; unattended merges
	 * (CLI, copy-all) must never silently overwrite the changed version.
	 */
	bool allowAdoptAgreed = false;
};

struct DefinitionInfo
{
	TagRange tag;
	int pane = -1;
	String text;
};

/** @brief A proposed safe semantic merge for one destination definition. */
struct Suggestion
{
	TagRange tags[3];
	DefinitionInfo defs[3];
	int unchangedPane = -1;
	int changedPane = -1;
	int srcPane = -1; /**< pane whose text is applied; equals changedPane unless adopting the agreed version */
	bool insertOnly = false;
	int insertLine = -1;
	int insertChar = 0;
	String replacementText;
	String previewText;
	String displayName;
};

/**
 * @brief Builds semantic merge suggestions for a 3-way comparison.
 *
 * The caller is responsible for option/read-only/diff-index validation;
 * the analyzer validates pane count and tree-sitter availability.
 */
class Analyzer
{
public:
	Analyzer(const PaneData* panes, int nPanes, const LanguageTraits& traits = LanguageTraits());

	/**
	 * @brief Build a suggestion for a single difference.
	 * @return true when a safe suggestion exists; otherwise false with an
	 *         explanatory message.
	 */
	bool TryBuildSuggestion(int dstPane, const DiffInfo& diff, Suggestion& suggestion, String& message) const;

	/**
	 * @brief Collect non-overlapping suggestions for the whole file.
	 * @param diffs  All significant differences, in document order.
	 *
	 * Suggestions are returned sorted by descending destination position so
	 * they can be applied in order without invalidating later ranges.
	 */
	bool TryCollectSuggestions(int dstPane, const std::vector<DiffInfo>& diffs, std::vector<Suggestion>& suggestions, String& message) const;

private:
	bool TryBuildInsertionSuggestion(int dstPane, const DiffInfo& diff, Suggestion& suggestion) const;
	std::vector<std::string> CollectCandidateDefinitionNames(const DiffInfo& diff) const;
	bool TryReconcileTagsByName(int dstPane, const DiffInfo& diff, Suggestion& suggestion, String& message) const;
	bool FinalizeStandardSuggestion(int dstPane, Suggestion& suggestion, String& message, bool allowAdoptAgreed = false) const;
	bool TryBuildCommentedBlockSuggestion(int dstPane, const DiffInfo& diff, Suggestion& suggestion, String& message) const;
	bool TryCollectIndependentAdditionSuggestions(int dstPane, std::vector<Suggestion>& suggestions, String& message) const;
	bool AllPanesHaveLanguage() const;

	const PaneData* m_panes;
	int m_nPanes;
	LanguageTraits m_traits;
};

// ---------------------------------------------------------------------------
// Internal building blocks, exposed for unit testing.
// ---------------------------------------------------------------------------

struct CommentedBlockInfo
{
	String linePrefix;
	String decommentedText;
};

/** @brief Strip CRs and trailing newlines for content comparison. */
String NormalizeSemanticText(String text);

/** @brief Find start offsets of whole-word occurrences of token in text. */
std::vector<size_t> FindWholeWordOccurrences(const String& text, const String& token);

/** @brief Replace whole-word occurrences; fails unless exactly expectedCount matches. */
bool ReplaceWholeWordOccurrences(const String& sourceText, const String& oldToken, const String& newToken,
	size_t expectedCount, String& resultText);

/** @brief Extract parameter names from a C-style function definition text. */
bool ExtractFunctionParameterNames(const String& text, std::vector<String>& parameterNames);

/** @brief Detect that exactly one quoted string literal differs between the two texts. */
bool ExtractSingleStringLiteralChange(const String& unchangedText, const String& changedText,
	const String& quoteChars, String& oldLiteralWithQuotes, String& newLiteralWithQuotes);

/** @brief Replace a string literal that occurs exactly once in sourceText. */
bool ReplaceUniqueStringLiteral(const String& sourceText, const String& oldLiteralWithQuotes,
	const String& newLiteralWithQuotes, String& resultText);

/** @brief Detect a single parameter rename with exactly one matching use-site update. */
bool ExtractSingleParameterRenameChange(const String& unchangedText, const String& changedText,
	String& oldParameterName, String& newParameterName);

/** @brief Detect a single identifier rename with 2-4 matching references. */
bool ExtractSingleIdentifierRenameChange(const String& unchangedText, const String& changedText,
	String& oldIdentifierName, String& newIdentifierName, size_t& occurrenceCount);

/** @brief Parse a block where every non-blank line starts with the same line-comment prefix. */
bool TryParseLineCommentedBlock(const String& text, const std::vector<String>& lineCommentPrefixes, CommentedBlockInfo& info);

/** @brief Render text as a line-commented block using the given prefix. */
String RenderLineCommentedBlock(const String& text, const String& prefix);

/** @brief Find the definition that best overlaps the given line range. */
bool TryFindBestTagForDiff(const std::vector<TagRange>& tags, int startLine, int endLine, TagRange& bestTag);

/** @brief Find the definition with the given name; fails if absent or ambiguous. */
bool TryFindUniqueTagByName(const std::vector<TagRange>& tags, const std::string& name, TagRange& found);

} // namespace SemanticMerge
