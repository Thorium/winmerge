/////////////////////////////////////////////////////////////////////////////
//    WinMerge:  an interactive diff/merge utility
//    SPDX-License-Identifier: GPL-2.0-or-later
/////////////////////////////////////////////////////////////////////////////
/**
 * @file  SemanticMergeAnalyzer.cpp
 *
 * @brief Implementation of the tree-sitter based semantic merge analyzer.
 */

#include "pch.h"
#include "SemanticMergeAnalyzer.h"
#include "MergeApp.h"
#include "unicoder.h"
#include <tchar.h>
#include <algorithm>
#include <unordered_map>

namespace SemanticMerge
{

LanguageTraits TraitsForLanguage(const String& languageName)
{
	String name = languageName;
	std::transform(name.begin(), name.end(), name.begin(),
		[](tchar_t ch) { return static_cast<tchar_t>(_totlower(ch)); });
	strutils::replace(name, _T("-"), _T("_"));

	LanguageTraits traits;
	const bool cFamily =
		name == _T("c") || name == _T("cpp") || name == _T("c_sharp") ||
		name == _T("java") || name == _T("javascript") || name == _T("typescript") ||
		name == _T("tsx") || name == _T("flow") || name == _T("php") || name == _T("php_only");
	const bool hashCommentFamily =
		name == _T("python") || name == _T("bash") || name == _T("ruby");
	const bool slashCommentOnly =
		name == _T("go") || name == _T("rust") || name == _T("fsharp") ||
		name == _T("fsharp_signature");

	if (cFamily)
	{
		traits.lineCommentPrefixes = { _T("//") };
		traits.cStyleParameters = true;
		traits.identifierRename = true;
		// Single quotes delimit char literals in C/C++/C#/Java, so only the
		// script-flavored members of the family get them as string quotes.
		if (name == _T("javascript") || name == _T("typescript") ||
			name == _T("tsx") || name == _T("flow") ||
			name == _T("php") || name == _T("php_only"))
			traits.stringQuoteChars = _T("\"'");
		else
			traits.stringQuoteChars = _T("\"");
		if (name == _T("php") || name == _T("php_only"))
			traits.lineCommentPrefixes.push_back(_T("#"));
	}
	else if (hashCommentFamily)
	{
		traits.lineCommentPrefixes = { _T("#") };
		traits.stringQuoteChars = _T("\"'");
		traits.identifierRename = true;
	}
	else if (slashCommentOnly)
	{
		traits.lineCommentPrefixes = { _T("//") };
		traits.stringQuoteChars = _T("\"");
		traits.identifierRename = true;
	}
	// Markup and data languages (json, html, css, xml, dtd) and unknown
	// languages keep the defaults: every text heuristic disabled.

	return traits;
}

// Helpers below have external linkage; the subset declared in the header is
// exposed for unit testing, the rest is internal to the analyzer.

enum class LocalizedReplayKind
{
	None,
	StringLiteral,
	ParameterRename,
	IdentifierRename,
};

bool GetDefinitionText(const ITextSource& src, const TagRange& tag, String& text)
{
	if (tag.startLine < 0 || tag.endLine < tag.startLine || tag.endLine >= src.GetLineCount())
		return false;

	if (tag.startChar < 0 || tag.startChar > src.GetLineLength(tag.startLine) ||
		tag.endChar < 0 || tag.endChar > src.GetLineLength(tag.endLine))
		return false;

	src.GetTextRange(tag.startLine, tag.startChar, tag.endLine, tag.endChar, text);
	return true;
}

bool GetLineRangeText(const ITextSource& src, int startLine, int endLine, String& text)
{
	if (startLine < 0 || endLine < startLine || endLine >= src.GetLineCount())
		return false;

	const int endChar = src.GetLineLength(endLine);
	src.GetTextRange(startLine, 0, endLine, endChar, text);
	text += src.GetLineEol(endLine);
	return true;
}

String NormalizeSemanticText(String text)
{
	strutils::replace(text, _T("\r"), _T(""));
	while (!text.empty() && text.back() == _T('\n'))
		text.pop_back();
	return text;
}

String TrimWhitespace(const String& text)
{
	size_t start = 0;
	while (start < text.size() && _istspace(text[start]))
		++start;

	size_t end = text.size();
	while (end > start && _istspace(text[end - 1]))
		--end;

	return text.substr(start, end - start);
}

bool IsIdentifierChar(tchar_t ch)
{
	return _istalnum(ch) || ch == _T('_');
}

bool ExtractTrailingIdentifier(const String& text, String& identifier)
{
	identifier.clear();
	size_t end = text.size();
	while (end > 0 && (_istspace(text[end - 1]) || text[end - 1] == _T('&') || text[end - 1] == _T('*')))
		--end;
	if (end == 0 || !IsIdentifierChar(text[end - 1]) || _istdigit(text[end - 1]))
		return false;

	size_t start = end - 1;
	while (start > 0 && IsIdentifierChar(text[start - 1]))
		--start;
	identifier = text.substr(start, end - start);
	return true;
}

bool ExtractFunctionParameterNames(const String& text, std::vector<String>& parameterNames)
{
	parameterNames.clear();
	const size_t bodyStart = text.find(_T('{'));
	const size_t openParen = text.find(_T('('));
	if (bodyStart == String::npos || openParen == String::npos || openParen > bodyStart)
		return false;

	int parenDepth = 0;
	size_t closeParen = String::npos;
	for (size_t i = openParen; i < bodyStart; ++i)
	{
		if (text[i] == _T('('))
			++parenDepth;
		else if (text[i] == _T(')'))
		{
			--parenDepth;
			if (parenDepth == 0)
			{
				closeParen = i;
				break;
			}
		}
	}
	if (closeParen == String::npos)
		return false;

	const String parametersText = text.substr(openParen + 1, closeParen - openParen - 1);
	std::vector<String> parameterDecls;
	String current;
	int nestedParens = 0;
	int nestedAngles = 0;
	int nestedBraces = 0;
	int nestedBrackets = 0;
	for (size_t i = 0; i < parametersText.size(); ++i)
	{
		const tchar_t ch = parametersText[i];
		switch (ch)
		{
		case _T('('): ++nestedParens; break;
		case _T(')'): if (nestedParens > 0) --nestedParens; break;
		case _T('<'): ++nestedAngles; break;
		case _T('>'): if (nestedAngles > 0) --nestedAngles; break;
		case _T('{'): ++nestedBraces; break;
		case _T('}'): if (nestedBraces > 0) --nestedBraces; break;
		case _T('['): ++nestedBrackets; break;
		case _T(']'): if (nestedBrackets > 0) --nestedBrackets; break;
		case _T(','):
			if (nestedParens == 0 && nestedAngles == 0 && nestedBraces == 0 && nestedBrackets == 0)
			{
				parameterDecls.push_back(current);
				current.clear();
				continue;
			}
			break;
		}
		current += ch;
	}
	parameterDecls.push_back(current);

	for (String parameterDecl : parameterDecls)
	{
		parameterDecl = TrimWhitespace(parameterDecl);
		if (parameterDecl.empty() || parameterDecl == _T("void"))
			continue;

		int defaultParens = 0;
		int defaultAngles = 0;
		int defaultBraces = 0;
		int defaultBrackets = 0;
		size_t defaultValuePos = String::npos;
		for (size_t i = 0; i < parameterDecl.size(); ++i)
		{
			const tchar_t ch = parameterDecl[i];
			switch (ch)
			{
			case _T('('): ++defaultParens; break;
			case _T(')'): if (defaultParens > 0) --defaultParens; break;
			case _T('<'): ++defaultAngles; break;
			case _T('>'): if (defaultAngles > 0) --defaultAngles; break;
			case _T('{'): ++defaultBraces; break;
			case _T('}'): if (defaultBraces > 0) --defaultBraces; break;
			case _T('['): ++defaultBrackets; break;
			case _T(']'): if (defaultBrackets > 0) --defaultBrackets; break;
			case _T('='):
				if (defaultParens == 0 && defaultAngles == 0 && defaultBraces == 0 && defaultBrackets == 0)
				{
					defaultValuePos = i;
					i = parameterDecl.size();
				}
				break;
			}
		}

		const String declarationWithoutDefault = TrimWhitespace(parameterDecl.substr(0, defaultValuePos));
		String parameterName;
		if (!ExtractTrailingIdentifier(declarationWithoutDefault, parameterName))
			return false;
		parameterNames.push_back(parameterName);
	}

	return true;
}

std::vector<size_t> FindWholeWordOccurrences(const String& text, const String& token)
{
	std::vector<size_t> positions;
	if (token.empty())
		return positions;

	for (size_t pos = text.find(token); pos != String::npos; pos = text.find(token, pos + 1))
	{
		const bool leftBoundary = (pos == 0) || !IsIdentifierChar(text[pos - 1]);
		const size_t end = pos + token.size();
		const bool rightBoundary = (end >= text.size()) || !IsIdentifierChar(text[end]);
		if (leftBoundary && rightBoundary)
			positions.push_back(pos);
	}

	return positions;
}

bool ReplaceWholeWordOccurrences(const String& sourceText, const String& oldToken, const String& newToken,
	size_t expectedCount, String& resultText)
{
	const std::vector<size_t> positions = FindWholeWordOccurrences(sourceText, oldToken);
	if (positions.size() != expectedCount)
		return false;

	resultText = sourceText;
	for (auto it = positions.rbegin(); it != positions.rend(); ++it)
		resultText.replace(*it, oldToken.size(), newToken);
	return true;
}

bool ExtractSingleStringLiteralChange(const String& unchangedText, const String& changedText,
	const String& quoteChars, String& oldLiteralWithQuotes, String& newLiteralWithQuotes)
{
	if (quoteChars.empty())
		return false;

	auto collectStringLiterals = [&quoteChars](const String& text)
	{
		std::vector<String> literals;
		for (size_t i = 0; i < text.size(); ++i)
		{
			if (quoteChars.find(text[i]) == String::npos)
				continue;

			const tchar_t quote = text[i];
			size_t j = i + 1;
			bool escaped = false;
			for (; j < text.size(); ++j)
			{
				if (escaped)
				{
					escaped = false;
					continue;
				}
				if (text[j] == _T('\\'))
				{
					escaped = true;
					continue;
				}
				if (text[j] == quote)
					break;
			}
			if (j >= text.size() || text[j] != quote)
				continue;

			literals.push_back(text.substr(i, j - i + 1));
			i = j;
		}
		return literals;
	};

	const std::vector<String> unchangedLiterals = collectStringLiterals(unchangedText);
	const std::vector<String> changedLiterals = collectStringLiterals(changedText);
	if (unchangedLiterals.empty() || changedLiterals.empty())
		return false;

	std::unordered_map<String, int> unchangedCounts;
	std::unordered_map<String, int> changedCounts;
	for (const auto& literal : unchangedLiterals)
		++unchangedCounts[literal];
	for (const auto& literal : changedLiterals)
		++changedCounts[literal];

	std::vector<String> removed;
	std::vector<String> added;
	for (const auto& entry : unchangedCounts)
	{
		const int delta = entry.second - changedCounts[entry.first];
		for (int i = 0; i < delta; ++i)
			removed.push_back(entry.first);
	}
	for (const auto& entry : changedCounts)
	{
		const int delta = entry.second - unchangedCounts[entry.first];
		for (int i = 0; i < delta; ++i)
			added.push_back(entry.first);
	}

	if (removed.size() != 1 || added.size() != 1)
		return false;

	oldLiteralWithQuotes = removed[0];
	newLiteralWithQuotes = added[0];
	return true;
}

bool ReplaceUniqueStringLiteral(const String& sourceText, const String& oldLiteralWithQuotes,
	const String& newLiteralWithQuotes, String& resultText)
{
	resultText = sourceText;
	const size_t first = resultText.find(oldLiteralWithQuotes);
	if (first == String::npos)
		return false;
	if (resultText.find(oldLiteralWithQuotes, first + oldLiteralWithQuotes.size()) != String::npos)
		return false;

	resultText.replace(first, oldLiteralWithQuotes.size(), newLiteralWithQuotes);
	return true;
}

bool ExtractSingleParameterRenameChange(const String& unchangedText, const String& changedText,
	String& oldParameterName, String& newParameterName)
{
	std::vector<String> unchangedParameters;
	std::vector<String> changedParameters;
	if (!ExtractFunctionParameterNames(unchangedText, unchangedParameters) ||
		!ExtractFunctionParameterNames(changedText, changedParameters) ||
		unchangedParameters.size() != changedParameters.size())
	{
		return false;
	}

	int changedParameterIndex = -1;
	for (size_t i = 0; i < unchangedParameters.size(); ++i)
	{
		if (unchangedParameters[i] == changedParameters[i])
			continue;
		if (changedParameterIndex != -1)
			return false;
		changedParameterIndex = static_cast<int>(i);
	}
	if (changedParameterIndex == -1)
		return false;

	oldParameterName = unchangedParameters[changedParameterIndex];
	newParameterName = changedParameters[changedParameterIndex];
	if (oldParameterName.empty() || newParameterName.empty())
		return false;

	if (FindWholeWordOccurrences(unchangedText, oldParameterName).size() != 2 ||
		FindWholeWordOccurrences(changedText, newParameterName).size() != 2)
	{
		return false;
	}

	String revertedText;
	if (!ReplaceWholeWordOccurrences(changedText, newParameterName, oldParameterName, 2, revertedText))
		return false;

	return revertedText == unchangedText;
}

bool ExtractSingleIdentifierRenameChange(const String& unchangedText, const String& changedText,
	String& oldIdentifierName, String& newIdentifierName, size_t& occurrenceCount)
{
	oldIdentifierName.clear();
	newIdentifierName.clear();
	occurrenceCount = 0;

	auto collectIdentifiers = [](const String& text)
	{
		std::vector<String> identifiers;
		for (size_t i = 0; i < text.size(); )
		{
			if (!IsIdentifierChar(text[i]) || _istdigit(text[i]))
			{
				++i;
				continue;
			}

			size_t end = i + 1;
			while (end < text.size() && IsIdentifierChar(text[end]))
				++end;
			identifiers.push_back(text.substr(i, end - i));
			i = end;
		}
		return identifiers;
	};

	const std::vector<String> unchangedIdentifiers = collectIdentifiers(unchangedText);
	const std::vector<String> changedIdentifiers = collectIdentifiers(changedText);
	if (unchangedIdentifiers.empty() || changedIdentifiers.empty())
		return false;

	std::unordered_map<String, int> unchangedCounts;
	std::unordered_map<String, int> changedCounts;
	for (const auto& identifier : unchangedIdentifiers)
		++unchangedCounts[identifier];
	for (const auto& identifier : changedIdentifiers)
		++changedCounts[identifier];

	// A rename replaces every occurrence, so exactly one identifier name must
	// disappear completely and exactly one new name must appear, with equal
	// occurrence counts.
	std::vector<String> removed;
	std::vector<String> added;
	for (const auto& entry : unchangedCounts)
	{
		if (changedCounts.find(entry.first) == changedCounts.end())
			removed.push_back(entry.first);
	}
	for (const auto& entry : changedCounts)
	{
		if (unchangedCounts.find(entry.first) == unchangedCounts.end())
			added.push_back(entry.first);
	}

	if (removed.size() != 1 || added.size() != 1)
		return false;

	oldIdentifierName = removed[0];
	newIdentifierName = added[0];
	if (oldIdentifierName.empty() || newIdentifierName.empty() || oldIdentifierName == newIdentifierName)
		return false;
	if (unchangedCounts[oldIdentifierName] != changedCounts[newIdentifierName])
		return false;

	const std::vector<size_t> oldPositions = FindWholeWordOccurrences(unchangedText, oldIdentifierName);
	const std::vector<size_t> newPositions = FindWholeWordOccurrences(changedText, newIdentifierName);
	if (oldPositions.size() != newPositions.size() || oldPositions.size() < 2 || oldPositions.size() > 4)
		return false;

	String revertedText;
	if (!ReplaceWholeWordOccurrences(changedText, newIdentifierName, oldIdentifierName, newPositions.size(), revertedText))
		return false;
	if (revertedText != unchangedText)
		return false;

	occurrenceCount = newPositions.size();
	return true;
}

bool TryDetectLineCommentPrefix(const String& line, const std::vector<String>& tokens, String& prefix, String& uncommented)
{
	size_t indent = 0;
	while (indent < line.size() && _istspace(line[indent]) && line[indent] != _T('\n') && line[indent] != _T('\r'))
		++indent;

	for (const String& token : tokens)
	{
		const size_t tokenLen = token.size();
		if (tokenLen == 0 || indent + tokenLen > line.size())
			continue;
		if (line.compare(indent, tokenLen, token) != 0)
			continue;

		size_t afterToken = indent + tokenLen;
		if (afterToken < line.size() && line[afterToken] == _T(' '))
			++afterToken;

		prefix = line.substr(0, afterToken);
		uncommented = line.substr(afterToken);
		return true;
	}

	return false;
}

bool TryParseLineCommentedBlock(const String& text, const std::vector<String>& lineCommentPrefixes, CommentedBlockInfo& info)
{
	info = {};
	if (lineCommentPrefixes.empty())
		return false;
	String detectedPrefix;
	String decommented;
	bool foundNonEmptyLine = false;

	size_t pos = 0;
	while (pos <= text.size())
	{
		size_t end = text.find(_T('\n'), pos);
		const bool hasNewline = (end != String::npos);
		String line = hasNewline ? text.substr(pos, end - pos) : text.substr(pos);
		if (!line.empty() && line.back() == _T('\r'))
			line.pop_back();

		bool blank = true;
		for (tchar_t ch : line)
		{
			if (!_istspace(ch))
			{
				blank = false;
				break;
			}
		}

		if (blank)
		{
			decommented += _T("\n");
		}
		else
		{
			String prefix;
			String uncommented;
			if (!TryDetectLineCommentPrefix(line, lineCommentPrefixes, prefix, uncommented))
				return false;
			if (!foundNonEmptyLine)
			{
				detectedPrefix = prefix;
				foundNonEmptyLine = true;
			}
			decommented += uncommented;
			decommented += _T("\n");
		}

		if (!hasNewline)
			break;
		pos = end + 1;
	}

	if (!foundNonEmptyLine)
		return false;

	info.linePrefix = detectedPrefix;
	info.decommentedText = decommented;
	return true;
}

String RenderLineCommentedBlock(const String& text, const String& prefix)
{
	String result;
	size_t pos = 0;
	while (pos <= text.size())
	{
		size_t end = text.find(_T('\n'), pos);
		const bool hasNewline = (end != String::npos);
		String line = hasNewline ? text.substr(pos, end - pos) : text.substr(pos);
		if (!line.empty() && line.back() == _T('\r'))
			line.pop_back();

		result += prefix;
		result += line;
		if (hasNewline || pos < text.size())
			result += _T("\n");

		if (!hasNewline)
			break;
		pos = end + 1;
	}
	return result;
}

int GetOverlapLength(const TagRange& tag, int startLine, int endLine)
{
	const int overlapStart = (std::max)(tag.startLine, startLine);
	const int overlapEnd = (std::min)(tag.endLine, endLine);
	return overlapEnd >= overlapStart ? (overlapEnd - overlapStart + 1) : 0;
}

bool TryFindBestTagForDiff(const std::vector<TagRange>& tags, int startLine, int endLine, TagRange& bestTag)
{
	if (tags.empty())
		return false;

	std::unordered_map<std::string, int> overlapByName;
	for (const auto& tag : tags)
	{
		const int overlap = GetOverlapLength(tag, startLine, endLine);
		if (overlap > 0)
			overlapByName[tag.name] += overlap;
	}

	if (overlapByName.empty())
		return false;

	std::string bestName;
	int bestNameScore = -1;
	for (const auto& entry : overlapByName)
	{
		if (entry.second > bestNameScore)
		{
			bestName = entry.first;
			bestNameScore = entry.second;
		}
	}

	bool found = false;
	int bestOverlap = -1;
	for (const auto& tag : tags)
	{
		if (tag.name != bestName)
			continue;
		const int overlap = GetOverlapLength(tag, startLine, endLine);
		if (overlap > bestOverlap)
		{
			bestTag = tag;
			bestOverlap = overlap;
			found = true;
		}
	}

	return found;
}

bool HaveSameDestinationDefinition(const Suggestion& left, const Suggestion& right, int dstPane)
{
	if (left.insertOnly || right.insertOnly)
	{
		return left.insertOnly == right.insertOnly &&
			left.insertLine == right.insertLine &&
			left.insertChar == right.insertChar;
	}

	const auto& leftTag = left.defs[dstPane].tag;
	const auto& rightTag = right.defs[dstPane].tag;
	return leftTag.startLine == rightTag.startLine &&
		leftTag.startChar == rightTag.startChar &&
		leftTag.endLine == rightTag.endLine &&
		leftTag.endChar == rightTag.endChar;
}

bool CompareSuggestionsByDescendingDestination(const Suggestion& left, const Suggestion& right, int dstPane)
{
	if (left.insertOnly != right.insertOnly)
		return left.insertOnly;
	if (left.insertOnly && right.insertOnly)
	{
		if (left.insertLine != right.insertLine)
			return left.insertLine > right.insertLine;
		return left.insertChar > right.insertChar;
	}

	const auto& leftTag = left.defs[dstPane].tag;
	const auto& rightTag = right.defs[dstPane].tag;
	if (leftTag.startLine != rightTag.startLine)
		return leftTag.startLine > rightTag.startLine;
	return leftTag.startChar > rightTag.startChar;
}

Analyzer::Analyzer(const PaneData* panes, int nPanes, const LanguageTraits& traits)
	: m_panes(panes)
	, m_nPanes(nPanes)
	, m_traits(traits)
{
}

bool Analyzer::AllPanesHaveLanguage() const
{
	for (int pane = 0; pane < m_nPanes; ++pane)
	{
		if (m_panes[pane].pText == nullptr || !m_panes[pane].hasLanguage)
			return false;
	}
	return m_nPanes > 0;
}

bool Analyzer::TryBuildInsertionSuggestion(int dstPane, const DiffInfo& diff, Suggestion& suggestion) const
{
	if (dstPane != 1)
		return false;
	if (diff.op != OP_1STONLY && diff.op != OP_3RDONLY)
		return false;

	const int addedPane = (diff.op == OP_1STONLY) ? 0 : 2;
	const std::vector<TagRange>& addedTags = m_panes[addedPane].tags;
	const std::vector<TagRange>& middleTags = m_panes[1].tags;
	if (addedTags.empty() || middleTags.empty())
		return false;

	std::vector<TagRange> addedCandidates;
	for (const auto& tag : addedTags)
	{
		if (GetOverlapLength(tag, diff.dbegin, diff.dend) > 0)
			addedCandidates.push_back(tag);
	}
	if (addedCandidates.size() != 1)
		return false;

	const auto& addedTag = addedCandidates[0];
	for (const auto& tag : middleTags)
	{
		if (tag.name == addedTag.name)
			return false;
	}

	String addedText;
	if (!GetDefinitionText(*m_panes[addedPane].pText, addedTag, addedText))
		return false;

	const TagRange* prevAddedTag = nullptr;
	const TagRange* nextAddedTag = nullptr;
	for (const auto& tag : addedTags)
	{
		if (tag.endLine < addedTag.startLine)
		{
			if (!prevAddedTag || tag.endLine > prevAddedTag->endLine)
				prevAddedTag = &tag;
		}
		else if (tag.startLine > addedTag.endLine)
		{
			if (!nextAddedTag || tag.startLine < nextAddedTag->startLine)
				nextAddedTag = &tag;
		}
	}

	const TagRange* prevMiddleTag = nullptr;
	const TagRange* nextMiddleTag = nullptr;
	if (prevAddedTag)
	{
		for (const auto& tag : middleTags)
		{
			if (tag.name == prevAddedTag->name)
			{
				prevMiddleTag = &tag;
				break;
			}
		}
	}
	if (nextAddedTag)
	{
		for (const auto& tag : middleTags)
		{
			if (tag.name == nextAddedTag->name)
			{
				nextMiddleTag = &tag;
				break;
			}
		}
	}

	if (!prevMiddleTag && !nextMiddleTag)
		return false;

	int insertLine = 0;
	int insertChar = 0;
	String insertedText = addedText;
	if (nextMiddleTag)
	{
		insertLine = nextMiddleTag->startLine;
		insertChar = nextMiddleTag->startChar;
		insertedText += _T("\n");
	}
	else
	{
		insertLine = prevMiddleTag->endLine;
		insertChar = prevMiddleTag->endChar;
		insertedText = _T("\n") + insertedText;
	}

	suggestion.changedPane = addedPane;
	suggestion.defs[addedPane].pane = addedPane;
	suggestion.defs[addedPane].tag = addedTag;
	suggestion.defs[addedPane].text = addedText;
	suggestion.defs[dstPane].pane = dstPane;
	suggestion.displayName = ucr::toTString(addedTag.name);
	suggestion.insertOnly = true;
	suggestion.insertLine = insertLine;
	suggestion.insertChar = insertChar;
	suggestion.replacementText = insertedText;
	suggestion.previewText = insertedText;
	return true;
}

bool Analyzer::FinalizeStandardSuggestion(int dstPane, Suggestion& suggestion, String& message) const
{
	for (int pane = 0; pane < m_nPanes; ++pane)
	{
		suggestion.defs[pane].tag = suggestion.tags[pane];
		suggestion.defs[pane].pane = pane;
		if (!GetDefinitionText(*m_panes[pane].pText, suggestion.defs[pane].tag, suggestion.defs[pane].text))
		{
			message = _("Failed to extract the selected definition text for semantic merge.");
			return false;
		}
	}

	if (suggestion.defs[0].text == suggestion.defs[1].text && suggestion.defs[0].text != suggestion.defs[2].text)
	{
		suggestion.unchangedPane = 0;
		suggestion.changedPane = 2;
	}
	else if (suggestion.defs[0].text == suggestion.defs[2].text && suggestion.defs[0].text != suggestion.defs[1].text)
	{
		suggestion.unchangedPane = 0;
		suggestion.changedPane = 1;
	}
	else if (suggestion.defs[1].text == suggestion.defs[2].text && suggestion.defs[1].text != suggestion.defs[0].text)
	{
		suggestion.unchangedPane = 1;
		suggestion.changedPane = 0;
	}
	else
	{
		int unchangedPane = -1;
		int localizedChangePane = -1;
		int refactoredPane = -1;
		String oldToken;
		String newToken;
		size_t localizedOccurrenceCount = 0;
		LocalizedReplayKind localizedReplayKind = LocalizedReplayKind::None;

		for (int candidateUnchangedPane = 0; candidateUnchangedPane < m_nPanes; ++candidateUnchangedPane)
		{
			const int candidatePane1 = (candidateUnchangedPane + 1) % 3;
			const int candidatePane2 = (candidateUnchangedPane + 2) % 3;

			if (ExtractSingleStringLiteralChange(
					suggestion.defs[candidateUnchangedPane].text,
					suggestion.defs[candidatePane1].text,
					m_traits.stringQuoteChars,
					oldToken,
					newToken))
			{
				localizedReplayKind = LocalizedReplayKind::StringLiteral;
				localizedChangePane = candidatePane1;
				refactoredPane = candidatePane2;
			}
			else if (m_traits.cStyleParameters && ExtractSingleParameterRenameChange(
					suggestion.defs[candidateUnchangedPane].text,
					suggestion.defs[candidatePane1].text,
					oldToken,
					newToken))
			{
				localizedReplayKind = LocalizedReplayKind::ParameterRename;
				localizedChangePane = candidatePane1;
				refactoredPane = candidatePane2;
				localizedOccurrenceCount = 2;
			}
			else if (m_traits.identifierRename && ExtractSingleIdentifierRenameChange(
					suggestion.defs[candidateUnchangedPane].text,
					suggestion.defs[candidatePane1].text,
					oldToken,
					newToken,
					localizedOccurrenceCount))
			{
				localizedReplayKind = LocalizedReplayKind::IdentifierRename;
				localizedChangePane = candidatePane1;
				refactoredPane = candidatePane2;
			}
			else if (ExtractSingleStringLiteralChange(
					suggestion.defs[candidateUnchangedPane].text,
					suggestion.defs[candidatePane2].text,
					m_traits.stringQuoteChars,
					oldToken,
					newToken))
			{
				localizedReplayKind = LocalizedReplayKind::StringLiteral;
				localizedChangePane = candidatePane2;
				refactoredPane = candidatePane1;
			}
			else if (m_traits.cStyleParameters && ExtractSingleParameterRenameChange(
					suggestion.defs[candidateUnchangedPane].text,
					suggestion.defs[candidatePane2].text,
					oldToken,
					newToken))
			{
				localizedReplayKind = LocalizedReplayKind::ParameterRename;
				localizedChangePane = candidatePane2;
				refactoredPane = candidatePane1;
				localizedOccurrenceCount = 2;
			}
			else if (m_traits.identifierRename && ExtractSingleIdentifierRenameChange(
					suggestion.defs[candidateUnchangedPane].text,
					suggestion.defs[candidatePane2].text,
					oldToken,
					newToken,
					localizedOccurrenceCount))
			{
				localizedReplayKind = LocalizedReplayKind::IdentifierRename;
				localizedChangePane = candidatePane2;
				refactoredPane = candidatePane1;
			}
			else
			{
				continue;
			}

			unchangedPane = candidateUnchangedPane;
			break;
		}

		if (unchangedPane == -1)
		{
			message = _("This definition does not have a single unchanged version plus a single changed version, so no safe semantic merge suggestion is available.");
			return false;
		}

		if (dstPane != refactoredPane)
		{
			message = _("This suggestion only works when you apply it to the pane with the refactored version of the definition. The other changed pane must contain only one small semantic change, currently either a single string literal change, a single parameter rename with one matching use-site update, or one identifier rename with very few matching references.");
			return false;
		}

		String replayedText;
		bool replaySucceeded = false;
		if (localizedReplayKind == LocalizedReplayKind::StringLiteral)
		{
			replaySucceeded = ReplaceUniqueStringLiteral(
				suggestion.defs[refactoredPane].text,
				oldToken,
				newToken,
				replayedText);
		}
		else if (localizedReplayKind == LocalizedReplayKind::ParameterRename)
		{
			replaySucceeded = ReplaceWholeWordOccurrences(
				suggestion.defs[refactoredPane].text,
				oldToken,
				newToken,
				2,
				replayedText);
		}
		else if (localizedReplayKind == LocalizedReplayKind::IdentifierRename)
		{
			replaySucceeded = ReplaceWholeWordOccurrences(
				suggestion.defs[refactoredPane].text,
				oldToken,
				newToken,
				localizedOccurrenceCount,
				replayedText);
		}

		if (!replaySucceeded)
		{
			message = _("The localized semantic update could not be mapped uniquely onto the destination definition.");
			return false;
		}

		suggestion.unchangedPane = unchangedPane;
		suggestion.changedPane = localizedChangePane;
		suggestion.displayName = ucr::toTString(suggestion.tags[0].name);
		suggestion.replacementText = replayedText;
		suggestion.previewText = replayedText;
		return true;
	}

	if (dstPane != suggestion.unchangedPane)
	{
		message = _("Semantic merge suggestion is currently only available when the destination pane still has the unchanged definition text.");
		return false;
	}

	if (suggestion.defs[dstPane].text == suggestion.defs[suggestion.changedPane].text)
	{
		message = _("The destination pane already contains the latest semantic definition text.");
		return false;
	}

	suggestion.displayName = ucr::toTString(suggestion.tags[0].name);
	suggestion.replacementText = suggestion.defs[suggestion.changedPane].text;
	suggestion.previewText = suggestion.defs[suggestion.changedPane].text;
	return true;
}

bool Analyzer::TryBuildCommentedBlockSuggestion(int dstPane, const DiffInfo& diff, Suggestion& suggestion, String& message) const
{
	// The destination pane holds the commented-out copy, so it has no tag for
	// the definition. The two other panes must agree on the definition name:
	// one still has the original text (matching the decommented block) and
	// the other has the updated text to replay into the comment.
	const int otherPane1 = (dstPane + 1) % 3;
	const int otherPane2 = (dstPane + 2) % 3;
	if (!TryFindBestTagForDiff(m_panes[otherPane1].tags, diff.dbegin, diff.dend, suggestion.tags[otherPane1]) ||
		!TryFindBestTagForDiff(m_panes[otherPane2].tags, diff.dbegin, diff.dend, suggestion.tags[otherPane2]))
	{
		message = _("No matching top-level definition was found for the selected difference in all three panes.");
		return false;
	}

	if (suggestion.tags[otherPane1].name != suggestion.tags[otherPane2].name)
	{
		message = _("The selected difference maps to different definitions across panes, so no safe semantic merge suggestion is available.");
		return false;
	}

	String text1;
	String text2;
	if (!GetDefinitionText(*m_panes[otherPane1].pText, suggestion.tags[otherPane1], text1) ||
		!GetDefinitionText(*m_panes[otherPane2].pText, suggestion.tags[otherPane2], text2))
	{
		message = _("Failed to extract the selected definition text for semantic merge.");
		return false;
	}

	String commentedBlockText;
	if (!GetLineRangeText(*m_panes[dstPane].pText, diff.dbegin, diff.dend, commentedBlockText))
	{
		message = _("Failed to extract the commented-out block for semantic merge.");
		return false;
	}

	CommentedBlockInfo commentInfo;
	if (!TryParseLineCommentedBlock(commentedBlockText, m_traits.lineCommentPrefixes, commentInfo))
	{
		message = _("No matching top-level definition was found for the selected difference in all three panes.");
		return false;
	}

	const String normalizedCommented = NormalizeSemanticText(commentInfo.decommentedText);
	const String normalized1 = NormalizeSemanticText(text1);
	const String normalized2 = NormalizeSemanticText(text2);

	if (normalized1 == normalized2)
	{
		message = _("The destination pane already contains the latest semantic definition text.");
		return false;
	}

	int unchangedPane;
	int changedPane;
	if (normalizedCommented == normalized1)
	{
		unchangedPane = otherPane1;
		changedPane = otherPane2;
	}
	else if (normalizedCommented == normalized2)
	{
		unchangedPane = otherPane2;
		changedPane = otherPane1;
	}
	else
	{
		message = _("The destination block looks commented out, but it does not match the unchanged function text closely enough for a safe semantic update.");
		return false;
	}

	const String& changedText = (changedPane == otherPane1) ? text1 : text2;

	// The destination has no tag, so the replacement target is the commented
	// block itself: the full diff line range, excluding the final EOL.
	TagRange dstRange{};
	dstRange.name = suggestion.tags[unchangedPane].name;
	dstRange.startLine = diff.dbegin;
	dstRange.startChar = 0;
	dstRange.endLine = diff.dend;
	dstRange.endChar = m_panes[dstPane].pText->GetLineLength(diff.dend);
	suggestion.tags[dstPane] = dstRange;

	String replacement = RenderLineCommentedBlock(changedText, commentInfo.linePrefix);
	while (!replacement.empty() && replacement.back() == _T('\n'))
		replacement.pop_back();

	for (int pane = 0; pane < m_nPanes; ++pane)
	{
		suggestion.defs[pane].tag = suggestion.tags[pane];
		suggestion.defs[pane].pane = pane;
	}
	suggestion.defs[unchangedPane].text = (unchangedPane == otherPane1) ? text1 : text2;
	suggestion.defs[changedPane].text = changedText;

	suggestion.unchangedPane = unchangedPane;
	suggestion.changedPane = changedPane;
	suggestion.displayName = ucr::toTString(suggestion.tags[unchangedPane].name);
	suggestion.replacementText = replacement;
	suggestion.previewText = replacement;
	return true;
}

bool Analyzer::TryBuildSuggestion(int dstPane, const DiffInfo& diff, Suggestion& suggestion, String& message) const
{
	message.clear();
	if (m_nPanes != 3)
	{
		message = _("Semantic merge suggestions currently support only 3-way text comparisons.");
		return false;
	}

	if (dstPane < 0 || dstPane >= m_nPanes)
	{
		message = _("The destination pane is read-only.");
		return false;
	}

	if (!AllPanesHaveLanguage())
	{
		message = _("Tree-sitter data is not available for all panes.");
		return false;
	}

	bool foundAllTags = true;
	for (int pane = 0; pane < m_nPanes; ++pane)
	{
		if (!TryFindBestTagForDiff(m_panes[pane].tags, diff.dbegin, diff.dend, suggestion.tags[pane]))
			foundAllTags = false;
	}

	if (foundAllTags)
	{
		if (suggestion.tags[0].name != suggestion.tags[1].name || suggestion.tags[0].name != suggestion.tags[2].name)
		{
			if (TryBuildInsertionSuggestion(dstPane, diff, suggestion))
				return true;
			message = _("The selected difference maps to different definitions across panes, so no safe semantic merge suggestion is available.");
			return false;
		}
		return FinalizeStandardSuggestion(dstPane, suggestion, message);
	}

	return TryBuildCommentedBlockSuggestion(dstPane, diff, suggestion, message);
}

bool Analyzer::TryCollectIndependentAdditionSuggestions(int dstPane, std::vector<Suggestion>& suggestions, String& message) const
{
	suggestions.clear();
	message.clear();
	if (dstPane != 1 || m_nPanes != 3 || !AllPanesHaveLanguage())
		return false;

	auto collectUniqueTagsByName = [](const std::vector<TagRange>& tags)
	{
		std::unordered_map<std::string, int> counts;
		for (const auto& tag : tags)
			++counts[tag.name];

		std::unordered_map<std::string, TagRange> uniqueTags;
		for (const auto& tag : tags)
		{
			if (counts[tag.name] == 1)
				uniqueTags[tag.name] = tag;
		}
		return uniqueTags;
	};

	const std::vector<TagRange>* paneTags[3] =
	{
		&m_panes[0].tags,
		&m_panes[1].tags,
		&m_panes[2].tags
	};
	if (paneTags[0]->empty() || paneTags[1]->empty() || paneTags[2]->empty())
		return false;

	const auto uniqueMiddleTags = collectUniqueTagsByName(*paneTags[1]);
	const auto uniqueLeftTags = collectUniqueTagsByName(*paneTags[0]);
	const auto uniqueRightTags = collectUniqueTagsByName(*paneTags[2]);

	struct AdditionGroup
	{
		int insertLine = -1;
		int insertChar = 0;
		bool beforeNext = true;
		std::vector<String> names;
		std::vector<String> texts;
	};

	std::unordered_map<String, AdditionGroup> groups;
	std::unordered_map<std::string, String> seenMissingTexts;

	auto collectFromSourcePane = [&](int srcPane, const auto& uniqueSourceTags) -> bool
	{
		for (size_t i = 0; i < paneTags[srcPane]->size(); ++i)
		{
			const auto& tag = (*paneTags[srcPane])[i];
			if (uniqueSourceTags.find(tag.name) == uniqueSourceTags.end())
				continue;
			if (uniqueMiddleTags.find(tag.name) != uniqueMiddleTags.end())
				continue;

			String addedText;
			if (!GetDefinitionText(*m_panes[srcPane].pText, tag, addedText))
				return false;

			auto seenIt = seenMissingTexts.find(tag.name);
			if (seenIt != seenMissingTexts.end())
			{
				if (seenIt->second != addedText)
				{
					message = _("Both changed panes add a definition with the same name but different contents, so no safe semantic copy suggestion is available.");
					return false;
				}
				continue;
			}

			const TagRange* prevMiddleTag = nullptr;
			const TagRange* nextMiddleTag = nullptr;
			for (size_t j = i; j-- > 0; )
			{
				auto prevIt = uniqueMiddleTags.find((*paneTags[srcPane])[j].name);
				if (prevIt != uniqueMiddleTags.end())
				{
					prevMiddleTag = &prevIt->second;
					break;
				}
			}
			for (size_t j = i + 1; j < paneTags[srcPane]->size(); ++j)
			{
				auto nextIt = uniqueMiddleTags.find((*paneTags[srcPane])[j].name);
				if (nextIt != uniqueMiddleTags.end())
				{
					nextMiddleTag = &nextIt->second;
					break;
				}
			}

			if (!prevMiddleTag && !nextMiddleTag)
				continue;

			int insertLine = 0;
			int insertChar = 0;
			bool beforeNext = true;
			String groupKey;
			if (nextMiddleTag)
			{
				insertLine = nextMiddleTag->startLine;
				insertChar = nextMiddleTag->startChar;
				groupKey = strutils::format(_T("B:%d:%d"), insertLine, insertChar);
			}
			else
			{
				insertLine = prevMiddleTag->endLine;
				insertChar = prevMiddleTag->endChar;
				beforeNext = false;
				groupKey = strutils::format(_T("A:%d:%d"), insertLine, insertChar);
			}

			AdditionGroup& group = groups[groupKey];
			if (group.insertLine == -1)
			{
				group.insertLine = insertLine;
				group.insertChar = insertChar;
				group.beforeNext = beforeNext;
			}
			group.names.push_back(ucr::toTString(tag.name));
			group.texts.push_back(addedText);
			seenMissingTexts[tag.name] = addedText;
		}
		return true;
	};

	if (!collectFromSourcePane(0, uniqueLeftTags) || !collectFromSourcePane(2, uniqueRightTags))
		return false;

	if (groups.empty())
		return false;

	for (const auto& entry : groups)
	{
		const AdditionGroup& group = entry.second;
		Suggestion suggestion;
		String combinedText;
		for (size_t i = 0; i < group.texts.size(); ++i)
		{
			if (i > 0)
				combinedText += _T("\n\n");
			combinedText += group.texts[i];
		}
		combinedText = group.beforeNext ? (combinedText + _T("\n\n")) : (_T("\n\n") + combinedText);

		suggestion.insertOnly = true;
		suggestion.insertLine = group.insertLine;
		suggestion.insertChar = group.insertChar;
		suggestion.defs[dstPane].pane = dstPane;
		suggestion.displayName = group.names.size() == 1 ? group.names[0] : _("added definitions");
		suggestion.replacementText = combinedText;
		suggestion.previewText = combinedText;
		suggestions.push_back(suggestion);
	}

	return true;
}

bool Analyzer::TryCollectSuggestions(int dstPane, const std::vector<DiffInfo>& diffs, std::vector<Suggestion>& suggestions, String& message) const
{
	suggestions.clear();
	message.clear();

	std::vector<Suggestion> collected;
	String additionsMessage;
	String lastMessage;
	for (const DiffInfo& diff : diffs)
	{
		Suggestion suggestion;
		String diffMessage;
		if (!TryBuildSuggestion(dstPane, diff, suggestion, diffMessage))
		{
			if (!diffMessage.empty())
				lastMessage = diffMessage;
			continue;
		}

		bool duplicate = false;
		for (const auto& existing : collected)
		{
			if (!HaveSameDestinationDefinition(existing, suggestion, dstPane))
				continue;
			if (existing.replacementText != suggestion.replacementText)
			{
				message = _("Multiple semantic suggestions target the same destination definition with different results, so a whole-file safe semantic copy is not available.");
				return false;
			}
			duplicate = true;
			break;
		}
		if (!duplicate)
			collected.push_back(suggestion);
	}

	if (collected.empty())
	{
		if (TryCollectIndependentAdditionSuggestions(dstPane, collected, additionsMessage) && !collected.empty())
		{
			std::sort(collected.begin(), collected.end(), [dstPane](const Suggestion& left, const Suggestion& right)
			{
				return CompareSuggestionsByDescendingDestination(left, right, dstPane);
			});
			suggestions.swap(collected);
			return true;
		}

		if (!additionsMessage.empty())
		{
			message = additionsMessage;
			return false;
		}

		message = !lastMessage.empty() ? lastMessage : _("No safe semantic copy suggestions are available for the destination pane.");
		return false;
	}

	std::sort(collected.begin(), collected.end(), [dstPane](const Suggestion& left, const Suggestion& right)
	{
		return CompareSuggestionsByDescendingDestination(left, right, dstPane);
	});

	for (size_t i = 1; i < collected.size(); ++i)
	{
		const auto& previous = collected[i - 1];
		const auto& current = collected[i];
		if (previous.insertOnly && current.insertOnly)
		{
			if (current.insertLine == previous.insertLine && current.insertChar == previous.insertChar)
			{
				message = _("Some semantic suggestions overlap in the destination pane, so a whole-file safe semantic copy is not available.");
				return false;
			}
			continue;
		}
		if (previous.insertOnly || current.insertOnly)
			continue;

		const auto& previousTag = previous.defs[dstPane].tag;
		const auto& currentTag = current.defs[dstPane].tag;
		if (currentTag.endLine > previousTag.startLine ||
			(currentTag.endLine == previousTag.startLine && currentTag.endChar > previousTag.startChar))
		{
			message = _("Some semantic suggestions overlap in the destination pane, so a whole-file safe semantic copy is not available.");
			return false;
		}
	}

	suggestions.swap(collected);
	return true;
}

} // namespace SemanticMerge
