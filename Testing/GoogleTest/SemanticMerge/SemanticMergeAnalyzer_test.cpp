/**
 * @file  SemanticMergeAnalyzer_test.cpp
 *
 * @brief Unit tests for the tree-sitter based semantic merge analyzer.
 *
 * The analyzer is exercised with synthetic pane data plus the manual test
 * triples in Testing/Data/SemanticMergeManual. Tags are produced by a small
 * scanner that mimics tree-sitter tags.scm output for the controlled C++
 * test inputs (top-level definitions starting at column 0).
 */
#include "pch.h"
#include <gtest/gtest.h>
#include "SemanticMergeAnalyzer.h"
#include "UnicodeString.h"
#include "unicoder.h"
#include <tchar.h>
#include <fstream>
#include <string>
#include <vector>

using SemanticMerge::TagRange;

namespace
{

/** @brief ITextSource over a plain vector of lines (LF line endings). */
class VectorTextSource : public SemanticMerge::ITextSource
{
public:
	explicit VectorTextSource(std::vector<String> lines) : m_lines(std::move(lines)) {}

	int GetLineCount() const override { return static_cast<int>(m_lines.size()); }
	int GetLineLength(int nLine) const override { return static_cast<int>(m_lines[nLine].size()); }

	void GetTextRange(int nStartLine, int nStartChar, int nEndLine, int nEndChar, String& text) const override
	{
		text.clear();
		for (int line = nStartLine; line <= nEndLine; ++line)
		{
			const String& lineText = m_lines[line];
			const int from = (line == nStartLine) ? nStartChar : 0;
			const int to = (line == nEndLine) ? nEndChar : static_cast<int>(lineText.size());
			text.append(lineText, from, to - from);
			if (line != nEndLine)
				text += _T('\n');
		}
	}

	String GetLineEol(int) const override { return _T("\n"); }

	const std::vector<String>& Lines() const { return m_lines; }

private:
	std::vector<String> m_lines;
};

bool IsIdentChar(tchar_t ch)
{
	return _istalnum(ch) || ch == _T('_');
}

/**
 * @brief Scan top-level function definitions the way tags.scm would report them.
 *
 * A definition starts at a column-0 line containing "name(" and ends at the
 * next line that consists of a single closing brace at column 0.
 */
std::vector<TagRange> ScanTopLevelFunctions(const std::vector<String>& lines)
{
	std::vector<TagRange> tags;
	for (size_t i = 0; i < lines.size(); ++i)
	{
		const String& line = lines[i];
		if (line.empty() || !(_istalpha(line[0]) || line[0] == _T('_')))
			continue;
		const size_t paren = line.find(_T('('));
		if (paren == String::npos || line.find(_T('{')) != String::npos)
			continue;

		size_t nameEnd = paren;
		while (nameEnd > 0 && line[nameEnd - 1] == _T(' '))
			--nameEnd;
		size_t nameBegin = nameEnd;
		while (nameBegin > 0 && IsIdentChar(line[nameBegin - 1]))
			--nameBegin;
		if (nameBegin == nameEnd)
			continue;

		size_t endLine = lines.size();
		for (size_t j = i + 1; j < lines.size(); ++j)
		{
			if (lines[j] == _T("}"))
			{
				endLine = j;
				break;
			}
		}
		if (endLine == lines.size())
			continue;

		TagRange tag{};
		tag.name = ucr::toUTF8(line.substr(nameBegin, nameEnd - nameBegin));
		tag.startLine = static_cast<int>(i);
		tag.startChar = 0;
		tag.endLine = static_cast<int>(endLine);
		tag.endChar = 1;
		tags.push_back(tag);
		i = endLine;
	}
	return tags;
}

bool LoadLines(const std::string& path, std::vector<String>& lines)
{
	std::ifstream file(path);
	if (!file)
		return false;
	lines.clear();
	std::string line;
	while (std::getline(file, line))
	{
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		lines.push_back(ucr::toTString(line));
	}
	return true;
}

/** @brief Holds three panes plus their analyzer, keeping sources alive. */
class ThreeWayFixture
{
public:
	ThreeWayFixture(std::vector<String> pane0, std::vector<String> pane1, std::vector<String> pane2)
	{
		std::vector<String> all[3] = { std::move(pane0), std::move(pane1), std::move(pane2) };
		for (int pane = 0; pane < 3; ++pane)
		{
			m_sources[pane] = std::make_unique<VectorTextSource>(std::move(all[pane]));
			m_panes[pane].pText = m_sources[pane].get();
			m_panes[pane].tags = ScanTopLevelFunctions(m_sources[pane]->Lines());
			m_panes[pane].hasLanguage = true;
		}
	}

	SemanticMerge::Analyzer MakeAnalyzer(const tchar_t* language = _T("cpp")) const
	{
		return SemanticMerge::Analyzer(m_panes, 3, SemanticMerge::TraitsForLanguage(language));
	}

	SemanticMerge::PaneData m_panes[3];

private:
	std::unique_ptr<VectorTextSource> m_sources[3];
};

SemanticMerge::DiffInfo MakeDiff(int dbegin, int dend, OP_TYPE op = OP_DIFF)
{
	SemanticMerge::DiffInfo diff;
	diff.dbegin = dbegin;
	diff.dend = dend;
	diff.op = op;
	return diff;
}

const std::string kDataDir = "../../Data/SemanticMergeManual/";

// ---------------------------------------------------------------------------
// Helper function tests
// ---------------------------------------------------------------------------

TEST(SemanticMergeHelpers, NormalizeSemanticTextStripsCrAndTrailingNewlines)
{
	EXPECT_EQ(_T("a\nb"), SemanticMerge::NormalizeSemanticText(_T("a\r\nb\r\n\n")));
	EXPECT_EQ(_T(""), SemanticMerge::NormalizeSemanticText(_T("\n\n")));
}

TEST(SemanticMergeHelpers, FindWholeWordOccurrencesRespectsBoundaries)
{
	const auto positions = SemanticMerge::FindWholeWordOccurrences(
		_T("score + scores + score_x + score"), _T("score"));
	ASSERT_EQ(2u, positions.size());
	EXPECT_EQ(0u, positions[0]);
	EXPECT_EQ(27u, positions[1]);
}

TEST(SemanticMergeHelpers, ReplaceWholeWordOccurrencesEnforcesExpectedCount)
{
	String result;
	EXPECT_FALSE(SemanticMerge::ReplaceWholeWordOccurrences(_T("a + a + a"), _T("a"), _T("b"), 2, result));
	EXPECT_TRUE(SemanticMerge::ReplaceWholeWordOccurrences(_T("a + a + a"), _T("a"), _T("b"), 3, result));
	EXPECT_EQ(_T("b + b + b"), result);
}

TEST(SemanticMergeHelpers, ExtractFunctionParameterNamesHandlesDefaultsAndNesting)
{
	std::vector<String> names;
	ASSERT_TRUE(SemanticMerge::ExtractFunctionParameterNames(
		_T("int Calc(int base, std::map<int, int> lookup, double scale = 1.0)\n{\n}"), names));
	ASSERT_EQ(3u, names.size());
	EXPECT_EQ(_T("base"), names[0]);
	EXPECT_EQ(_T("lookup"), names[1]);
	EXPECT_EQ(_T("scale"), names[2]);

	ASSERT_TRUE(SemanticMerge::ExtractFunctionParameterNames(_T("int Zero(void)\n{\n}"), names));
	EXPECT_TRUE(names.empty());
}

TEST(SemanticMergeHelpers, ExtractSingleStringLiteralChangeFindsUniqueChange)
{
	String oldLit, newLit;
	ASSERT_TRUE(SemanticMerge::ExtractSingleStringLiteralChange(
		_T("return \"old\"; log(\"same\");"),
		_T("return \"new\"; log(\"same\");"),
		_T("\""), oldLit, newLit));
	EXPECT_EQ(_T("\"old\""), oldLit);
	EXPECT_EQ(_T("\"new\""), newLit);

	// Two changed literals must be rejected.
	EXPECT_FALSE(SemanticMerge::ExtractSingleStringLiteralChange(
		_T("f(\"a\", \"b\");"), _T("f(\"x\", \"y\");"), _T("\""), oldLit, newLit));

	// Single-quoted literals are found when the language allows them.
	ASSERT_TRUE(SemanticMerge::ExtractSingleStringLiteralChange(
		_T("say('hi')"), _T("say('bye')"), _T("\"'"), oldLit, newLit));
	EXPECT_EQ(_T("'hi'"), oldLit);
	EXPECT_EQ(_T("'bye'"), newLit);
	EXPECT_FALSE(SemanticMerge::ExtractSingleStringLiteralChange(
		_T("say('hi')"), _T("say('bye')"), _T("\""), oldLit, newLit));
}

TEST(SemanticMergeHelpers, ReplaceUniqueStringLiteralRejectsAmbiguousTarget)
{
	String result;
	EXPECT_TRUE(SemanticMerge::ReplaceUniqueStringLiteral(
		_T("say(\"hi\");"), _T("\"hi\""), _T("\"bye\""), result));
	EXPECT_EQ(_T("say(\"bye\");"), result);

	EXPECT_FALSE(SemanticMerge::ReplaceUniqueStringLiteral(
		_T("say(\"hi\"); say(\"hi\");"), _T("\"hi\""), _T("\"bye\""), result));
}

TEST(SemanticMergeHelpers, ExtractSingleParameterRenameChangeRequiresExactPattern)
{
	const String before = _T("int Calc(int base)\n{\n\treturn base + 1;\n}");
	const String after = _T("int Calc(int offset)\n{\n\treturn offset + 1;\n}");
	String oldName, newName;
	ASSERT_TRUE(SemanticMerge::ExtractSingleParameterRenameChange(before, after, oldName, newName));
	EXPECT_EQ(_T("base"), oldName);
	EXPECT_EQ(_T("offset"), newName);

	// A rename combined with another edit must be rejected.
	const String afterWithEdit = _T("int Calc(int offset)\n{\n\treturn offset + 2;\n}");
	EXPECT_FALSE(SemanticMerge::ExtractSingleParameterRenameChange(before, afterWithEdit, oldName, newName));
}

TEST(SemanticMergeHelpers, ExtractSingleIdentifierRenameChangeLimitsOccurrences)
{
	const String before = _T("int f()\n{\n\tint score = 1;\n\tscore += 2;\n\treturn score;\n}");
	const String after = _T("int f()\n{\n\tint total = 1;\n\ttotal += 2;\n\treturn total;\n}");
	String oldName, newName;
	size_t count = 0;
	ASSERT_TRUE(SemanticMerge::ExtractSingleIdentifierRenameChange(before, after, oldName, newName, count));
	EXPECT_EQ(_T("score"), oldName);
	EXPECT_EQ(_T("total"), newName);
	EXPECT_EQ(3u, count);

	// More than 4 occurrences is rejected as too broad.
	const String beforeMany = _T("a;a;a;a;a;");
	const String afterMany = _T("b;b;b;b;b;");
	EXPECT_FALSE(SemanticMerge::ExtractSingleIdentifierRenameChange(beforeMany, afterMany, oldName, newName, count));
}

TEST(SemanticMergeHelpers, CommentedBlockRoundTrip)
{
	SemanticMerge::CommentedBlockInfo info;
	const std::vector<String> slashes = { _T("//") };
	ASSERT_TRUE(SemanticMerge::TryParseLineCommentedBlock(
		_T("// int f()\n// {\n// \treturn 1;\n// }\n"), slashes, info));
	EXPECT_EQ(_T("// "), info.linePrefix);
	// Trailing newlines are not significant; NormalizeSemanticText strips them
	// before the analyzer compares decommented text with definition text.
	EXPECT_EQ(_T("int f()\n{\n\treturn 1;\n}"),
		SemanticMerge::NormalizeSemanticText(info.decommentedText));

	const String rendered = SemanticMerge::RenderLineCommentedBlock(_T("int g()\n{\n}"), _T("// "));
	EXPECT_EQ(_T("// int g()\n// {\n// }"), SemanticMerge::NormalizeSemanticText(rendered));

	// Mixed comment styles are rejected.
	EXPECT_FALSE(SemanticMerge::TryParseLineCommentedBlock(_T("// a\nb\n"), slashes, info));
	// A prefix outside the language's comment set is rejected.
	EXPECT_FALSE(SemanticMerge::TryParseLineCommentedBlock(_T("# a\n# b\n"), slashes, info));
}

TEST(SemanticMergeHelpers, TryFindBestTagForDiffPicksLargestOverlap)
{
	std::vector<TagRange> tags(2);
	tags[0].name = "first";
	tags[0].startLine = 0;
	tags[0].endLine = 4;
	tags[1].name = "second";
	tags[1].startLine = 6;
	tags[1].endLine = 14;

	TagRange best{};
	ASSERT_TRUE(SemanticMerge::TryFindBestTagForDiff(tags, 3, 9, best));
	EXPECT_EQ("second", best.name);

	EXPECT_FALSE(SemanticMerge::TryFindBestTagForDiff(tags, 20, 25, best));
}

TEST(SemanticMergeHelpers, TraitsForLanguageMapping)
{
	const auto cpp = SemanticMerge::TraitsForLanguage(_T("cpp"));
	EXPECT_EQ(_T("\""), cpp.stringQuoteChars);
	EXPECT_TRUE(cpp.cStyleParameters);
	EXPECT_TRUE(cpp.identifierRename);
	ASSERT_EQ(1u, cpp.lineCommentPrefixes.size());
	EXPECT_EQ(_T("//"), cpp.lineCommentPrefixes[0]);

	const auto python = SemanticMerge::TraitsForLanguage(_T("python"));
	EXPECT_EQ(_T("\"'"), python.stringQuoteChars);
	EXPECT_FALSE(python.cStyleParameters);
	ASSERT_EQ(1u, python.lineCommentPrefixes.size());
	EXPECT_EQ(_T("#"), python.lineCommentPrefixes[0]);

	// Registry names may use dashes and mixed case.
	const auto csharp = SemanticMerge::TraitsForLanguage(_T("C-Sharp"));
	EXPECT_TRUE(csharp.cStyleParameters);

	// Markup/data and unknown languages get every heuristic disabled.
	for (const tchar_t* name : { _T("json"), _T("html"), _T("css"), _T("xml"), _T("unknown-lang") })
	{
		const auto traits = SemanticMerge::TraitsForLanguage(name);
		EXPECT_TRUE(traits.stringQuoteChars.empty()) << ucr::toUTF8(name);
		EXPECT_TRUE(traits.lineCommentPrefixes.empty()) << ucr::toUTF8(name);
		EXPECT_FALSE(traits.cStyleParameters) << ucr::toUTF8(name);
		EXPECT_FALSE(traits.identifierRename) << ucr::toUTF8(name);
	}
}

// ---------------------------------------------------------------------------
// Analyzer scenario tests (synthetic panes)
// ---------------------------------------------------------------------------

std::vector<String> SimpleFunction(const tchar_t* body)
{
	return {
		_T("int Compute(int value)"),
		_T("{"),
		String(_T("\t")) + body,
		_T("}"),
	};
}

TEST(SemanticMergeAnalyzer, StandardReplaceTakesChangedDefinition)
{
	ThreeWayFixture fx(
		SimpleFunction(_T("return value + 1;")),
		SimpleFunction(_T("return value + 1;")),
		SimpleFunction(_T("return value + 5;")));
	auto analyzer = fx.MakeAnalyzer();

	SemanticMerge::Suggestion suggestion;
	String message;
	ASSERT_TRUE(analyzer.TryBuildSuggestion(0, MakeDiff(2, 2), suggestion, message)) << ucr::toUTF8(message);
	EXPECT_EQ(0, suggestion.unchangedPane);
	EXPECT_EQ(2, suggestion.changedPane);
	EXPECT_FALSE(suggestion.insertOnly);
	EXPECT_NE(String::npos, suggestion.replacementText.find(_T("value + 5")));

	// Applying to the already-changed pane is rejected.
	EXPECT_FALSE(analyzer.TryBuildSuggestion(2, MakeDiff(2, 2), suggestion, message));
}

TEST(SemanticMergeAnalyzer, StringLiteralReplayOntoRefactoredPane)
{
	std::vector<String> base = {
		_T("const char* Msg(int value)"),
		_T("{"),
		_T("\tif (value > 0)"),
		_T("\t\treturn \"stable\";"),
		_T("\treturn \"fallback\";"),
		_T("}"),
	};
	std::vector<String> literalChanged = base;
	literalChanged[3] = _T("\t\treturn \"changed\";");
	std::vector<String> refactored = {
		_T("const char* Msg(int value)"),
		_T("{"),
		_T("\tconst bool hasValue = value > 0;"),
		_T("\tif (hasValue)"),
		_T("\t\treturn \"stable\";"),
		_T("\treturn \"fallback\";"),
		_T("}"),
	};

	ThreeWayFixture fx(base, literalChanged, refactored);
	auto analyzer = fx.MakeAnalyzer();

	SemanticMerge::Suggestion suggestion;
	String message;
	ASSERT_TRUE(analyzer.TryBuildSuggestion(2, MakeDiff(2, 4), suggestion, message)) << ucr::toUTF8(message);
	EXPECT_EQ(0, suggestion.unchangedPane);
	EXPECT_EQ(1, suggestion.changedPane);
	EXPECT_NE(String::npos, suggestion.replacementText.find(_T("\"changed\"")));
	EXPECT_NE(String::npos, suggestion.replacementText.find(_T("hasValue")));
	EXPECT_EQ(String::npos, suggestion.replacementText.find(_T("\"stable\"")));

	// The replay only applies onto the refactored pane.
	EXPECT_FALSE(analyzer.TryBuildSuggestion(1, MakeDiff(2, 4), suggestion, message));

	// A language without known string syntax disables the replay heuristics.
	auto gatedAnalyzer = fx.MakeAnalyzer(_T("unknown-lang"));
	EXPECT_FALSE(gatedAnalyzer.TryBuildSuggestion(2, MakeDiff(2, 4), suggestion, message));
}

TEST(SemanticMergeAnalyzer, ParameterRenameReplayOntoRefactoredPane)
{
	std::vector<String> base = {
		_T("int Calc(int base)"),
		_T("{"),
		_T("\treturn base + 1;"),
		_T("}"),
	};
	std::vector<String> renamed = {
		_T("int Calc(int offset)"),
		_T("{"),
		_T("\treturn offset + 1;"),
		_T("}"),
	};
	std::vector<String> refactored = {
		_T("int Calc(int base)"),
		_T("{"),
		_T("\tconst int result = base + 1;"),
		_T("\treturn result;"),
		_T("}"),
	};

	ThreeWayFixture fx(base, renamed, refactored);
	auto analyzer = fx.MakeAnalyzer();

	SemanticMerge::Suggestion suggestion;
	String message;
	ASSERT_TRUE(analyzer.TryBuildSuggestion(2, MakeDiff(0, 3), suggestion, message)) << ucr::toUTF8(message);
	EXPECT_NE(String::npos, suggestion.replacementText.find(_T("int Calc(int offset)")));
	EXPECT_NE(String::npos, suggestion.replacementText.find(_T("offset + 1")));
	EXPECT_NE(String::npos, suggestion.replacementText.find(_T("result")));
}

TEST(SemanticMergeAnalyzer, IdentifierRenameReplayOntoRefactoredPane)
{
	std::vector<String> base = {
		_T("int Calc(int value)"),
		_T("{"),
		_T("\tint score = value;"),
		_T("\tscore += 2;"),
		_T("\treturn score;"),
		_T("}"),
	};
	std::vector<String> renamed = {
		_T("int Calc(int value)"),
		_T("{"),
		_T("\tint total = value;"),
		_T("\ttotal += 2;"),
		_T("\treturn total;"),
		_T("}"),
	};
	std::vector<String> refactored = {
		_T("int Calc(int value)"),
		_T("{"),
		_T("\tint score = value;"),
		_T("\tscore += 2;"),
		_T("\tif (score > 10)"),
		_T("\t\treturn 10;"),
		_T("\treturn score;"),
		_T("}"),
	};

	ThreeWayFixture fx(base, renamed, refactored);
	auto analyzer = fx.MakeAnalyzer();

	SemanticMerge::Suggestion suggestion;
	String message;
	// The refactor added a fourth occurrence of "score", so the rename has 3
	// occurrences on the renamed side but 4 in the destination; the replay
	// must be rejected as not uniquely mappable.
	EXPECT_FALSE(analyzer.TryBuildSuggestion(2, MakeDiff(2, 5), suggestion, message));

	// With a refactor that keeps the occurrence count, the replay succeeds.
	std::vector<String> refactoredSameCount = {
		_T("int Calc(int value)"),
		_T("{"),
		_T("\tconst int bonus = 2;"),
		_T("\tint score = value;"),
		_T("\tscore += bonus;"),
		_T("\treturn score;"),
		_T("}"),
	};
	ThreeWayFixture fx2(base, renamed, refactoredSameCount);
	auto analyzer2 = fx2.MakeAnalyzer();
	ASSERT_TRUE(analyzer2.TryBuildSuggestion(2, MakeDiff(2, 5), suggestion, message)) << ucr::toUTF8(message);
	EXPECT_NE(String::npos, suggestion.replacementText.find(_T("total += bonus")));
	EXPECT_EQ(String::npos, suggestion.replacementText.find(_T("score")));
}

TEST(SemanticMergeAnalyzer, OneSidedAdditionInsertsIntoMiddle)
{
	std::vector<String> withExtra = {
		_T("int First(int a)"),
		_T("{"),
		_T("\treturn a;"),
		_T("}"),
		_T("int Extra(int b)"),
		_T("{"),
		_T("\treturn b * 2;"),
		_T("}"),
		_T("int Last(int c)"),
		_T("{"),
		_T("\treturn c;"),
		_T("}"),
	};
	std::vector<String> without = {
		_T("int First(int a)"),
		_T("{"),
		_T("\treturn a;"),
		_T("}"),
		_T("int Last(int c)"),
		_T("{"),
		_T("\treturn c;"),
		_T("}"),
	};

	ThreeWayFixture fx(withExtra, without, without);
	auto analyzer = fx.MakeAnalyzer();

	SemanticMerge::Suggestion suggestion;
	String message;
	ASSERT_TRUE(analyzer.TryBuildSuggestion(1, MakeDiff(4, 7, OP_1STONLY), suggestion, message)) << ucr::toUTF8(message);
	EXPECT_TRUE(suggestion.insertOnly);
	EXPECT_EQ(4, suggestion.insertLine); // before Last() in the middle pane
	EXPECT_NE(String::npos, suggestion.replacementText.find(_T("int Extra(int b)")));
}

TEST(SemanticMergeAnalyzer, IndependentAdditionsCollectedForMiddlePane)
{
	std::vector<String> common = {
		_T("int First(int a)"),
		_T("{"),
		_T("\treturn a;"),
		_T("}"),
		_T("int Last(int c)"),
		_T("{"),
		_T("\treturn c;"),
		_T("}"),
	};
	std::vector<String> left = {
		_T("int First(int a)"),
		_T("{"),
		_T("\treturn a;"),
		_T("}"),
		_T("int LeftOnly(int x)"),
		_T("{"),
		_T("\treturn x + 1;"),
		_T("}"),
		_T("int Last(int c)"),
		_T("{"),
		_T("\treturn c;"),
		_T("}"),
	};
	std::vector<String> right = {
		_T("int First(int a)"),
		_T("{"),
		_T("\treturn a;"),
		_T("}"),
		_T("int Last(int c)"),
		_T("{"),
		_T("\treturn c;"),
		_T("}"),
		_T("int RightOnly(int y)"),
		_T("{"),
		_T("\treturn y - 1;"),
		_T("}"),
	};

	ThreeWayFixture fx(left, common, right);
	auto analyzer = fx.MakeAnalyzer();

	std::vector<SemanticMerge::Suggestion> suggestions;
	String message;
	ASSERT_TRUE(analyzer.TryCollectSuggestions(1, {}, suggestions, message)) << ucr::toUTF8(message);
	ASSERT_EQ(2u, suggestions.size());
	String combined;
	for (const auto& s : suggestions)
	{
		EXPECT_TRUE(s.insertOnly);
		combined += s.replacementText;
	}
	EXPECT_NE(String::npos, combined.find(_T("LeftOnly")));
	EXPECT_NE(String::npos, combined.find(_T("RightOnly")));
}

TEST(SemanticMergeAnalyzer, BothSidesChangedSameDefinitionIsRejected)
{
	ThreeWayFixture fx(
		SimpleFunction(_T("return value + 1;")),
		SimpleFunction(_T("return value + 2;")),
		SimpleFunction(_T("return value + 3;")));
	auto analyzer = fx.MakeAnalyzer();

	SemanticMerge::Suggestion suggestion;
	String message;
	EXPECT_FALSE(analyzer.TryBuildSuggestion(0, MakeDiff(2, 2), suggestion, message));
	EXPECT_FALSE(message.empty());
}

TEST(SemanticMergeAnalyzer, MissingTreeSitterDataIsRejected)
{
	ThreeWayFixture fx(
		SimpleFunction(_T("return value;")),
		SimpleFunction(_T("return value;")),
		SimpleFunction(_T("return value + 1;")));
	fx.m_panes[1].hasLanguage = false;
	SemanticMerge::Analyzer analyzer(fx.m_panes, 3);

	SemanticMerge::Suggestion suggestion;
	String message;
	EXPECT_FALSE(analyzer.TryBuildSuggestion(0, MakeDiff(2, 2), suggestion, message));
	EXPECT_FALSE(message.empty());
}

TEST(SemanticMergeAnalyzer, TwoWayComparisonIsRejected)
{
	ThreeWayFixture fx(
		SimpleFunction(_T("return value;")),
		SimpleFunction(_T("return value;")),
		SimpleFunction(_T("return value;")));
	SemanticMerge::Analyzer analyzer(fx.m_panes, 2);

	SemanticMerge::Suggestion suggestion;
	String message;
	EXPECT_FALSE(analyzer.TryBuildSuggestion(0, MakeDiff(0, 1), suggestion, message));
	EXPECT_FALSE(message.empty());
}

TEST(SemanticMergeAnalyzer, CommentedOutFunctionReceivesUpdatedContent)
{
	std::vector<String> commentedOut = {
		_T("// int Compute(int value)"),
		_T("// {"),
		_T("// \treturn value + 1;"),
		_T("// }"),
	};

	ThreeWayFixture fx(
		commentedOut,
		SimpleFunction(_T("return value + 1;")),
		SimpleFunction(_T("return value + 5;")));
	auto analyzer = fx.MakeAnalyzer();

	SemanticMerge::Suggestion suggestion;
	String message;
	ASSERT_TRUE(analyzer.TryBuildSuggestion(0, MakeDiff(0, 3), suggestion, message)) << ucr::toUTF8(message);
	EXPECT_EQ(1, suggestion.unchangedPane);
	EXPECT_EQ(2, suggestion.changedPane);
	EXPECT_FALSE(suggestion.insertOnly);
	EXPECT_EQ(_T("// int Compute(int value)\n// {\n// \treturn value + 5;\n// }"),
		suggestion.replacementText);
	// The destination range covers the commented block.
	EXPECT_EQ(0, suggestion.defs[0].tag.startLine);
	EXPECT_EQ(3, suggestion.defs[0].tag.endLine);
}

TEST(SemanticMergeAnalyzer, CommentedBlockWithManualNotesIsRejected)
{
	std::vector<String> commentedOut = {
		_T("// NOTE: disabled until the new scoring ships"),
		_T("// int Compute(int value)"),
		_T("// {"),
		_T("// \treturn value + 1;"),
		_T("// }"),
	};

	ThreeWayFixture fx(
		commentedOut,
		SimpleFunction(_T("return value + 1;")),
		SimpleFunction(_T("return value + 5;")));
	auto analyzer = fx.MakeAnalyzer();

	SemanticMerge::Suggestion suggestion;
	String message;
	EXPECT_FALSE(analyzer.TryBuildSuggestion(0, MakeDiff(0, 4), suggestion, message));
	EXPECT_FALSE(message.empty());
}

TEST(SemanticMergeAnalyzer, CommentedOutFunctionInRightPaneAlsoSupported)
{
	std::vector<String> commentedOut = {
		_T("# def compute(value)"),
		_T("# \treturn value + 1"),
	};
	std::vector<String> original = {
		_T("def compute(value)"),
		_T("\treturn value + 1"),
	};
	std::vector<String> updated = {
		_T("def compute(value)"),
		_T("\treturn value + 5"),
	};

	// Tags scanned from python-style snippets will not match the C++ scanner,
	// so hand-build them: panes 0 and 1 hold the definition, pane 2 holds the
	// commented copy.
	ThreeWayFixture fx(updated, original, commentedOut);
	for (int pane = 0; pane < 2; ++pane)
	{
		TagRange tag{};
		tag.name = "compute";
		tag.startLine = 0;
		tag.startChar = 0;
		tag.endLine = 1;
		tag.endChar = static_cast<int>(original[1].size());
		fx.m_panes[pane].tags = { tag };
	}
	fx.m_panes[2].tags.clear();
	auto analyzer = fx.MakeAnalyzer(_T("python"));

	SemanticMerge::Suggestion suggestion;
	String message;
	ASSERT_TRUE(analyzer.TryBuildSuggestion(2, MakeDiff(0, 1), suggestion, message)) << ucr::toUTF8(message);
	EXPECT_EQ(1, suggestion.unchangedPane);
	EXPECT_EQ(0, suggestion.changedPane);
	EXPECT_EQ(_T("# def compute(value)\n# \treturn value + 5"), suggestion.replacementText);
}

// ---------------------------------------------------------------------------
// File-based scenario tests over Testing/Data/SemanticMergeManual
// ---------------------------------------------------------------------------

TEST(SemanticMergeManualData, StringLiteralTripleReplaysOntoRight)
{
	std::vector<String> left, middle, right;
	ASSERT_TRUE(LoadLines(kDataDir + "SemanticStringLiteral3WayLeft.cpp", left));
	ASSERT_TRUE(LoadLines(kDataDir + "SemanticStringLiteral3WayMiddle.cpp", middle));
	ASSERT_TRUE(LoadLines(kDataDir + "SemanticStringLiteral3WayRight.cpp", right));

	ThreeWayFixture fx(left, middle, right);
	for (int pane = 0; pane < 3; ++pane)
		ASSERT_EQ(1u, fx.m_panes[pane].tags.size()) << "pane " << pane;
	auto analyzer = fx.MakeAnalyzer();

	SemanticMerge::Suggestion suggestion;
	String message;
	ASSERT_TRUE(analyzer.TryBuildSuggestion(2, MakeDiff(2, 6), suggestion, message)) << ucr::toUTF8(message);
	EXPECT_NE(String::npos, suggestion.replacementText.find(_T("\"changed version\"")));
	EXPECT_NE(String::npos, suggestion.replacementText.find(_T("hasValue")));
	EXPECT_EQ(String::npos, suggestion.replacementText.find(_T("\"stable version\"")));
}

TEST(SemanticMergeManualData, CommentedTripleUpdatesCommentedCopy)
{
	std::vector<String> left, middle, right;
	ASSERT_TRUE(LoadLines(kDataDir + "SemanticCommented3WayLeft.cpp", left));
	ASSERT_TRUE(LoadLines(kDataDir + "SemanticCommented3WayMiddle.cpp", middle));
	ASSERT_TRUE(LoadLines(kDataDir + "SemanticCommented3WayRight.cpp", right));

	ThreeWayFixture fx(left, middle, right);
	EXPECT_EQ(1u, fx.m_panes[0].tags.size()); // only BaseValue; ComputeScore is commented out
	ASSERT_EQ(2u, fx.m_panes[1].tags.size());
	ASSERT_EQ(2u, fx.m_panes[2].tags.size());
	auto analyzer = fx.MakeAnalyzer();

	SemanticMerge::Suggestion suggestion;
	String message;
	ASSERT_TRUE(analyzer.TryBuildSuggestion(0, MakeDiff(7, 10), suggestion, message)) << ucr::toUTF8(message);
	EXPECT_EQ(1, suggestion.unchangedPane);
	EXPECT_EQ(2, suggestion.changedPane);
	EXPECT_EQ(_T("// int ComputeScore(int base)\n// {\n// \treturn base + 2;\n// }"),
		suggestion.replacementText);
}

TEST(SemanticMergeManualData, CliTripleStandardReplace)
{
	std::vector<String> left, middle, right;
	ASSERT_TRUE(LoadLines(kDataDir + "SemanticCli3WayLeft.cpp", left));
	ASSERT_TRUE(LoadLines(kDataDir + "SemanticCli3WayMiddle.cpp", middle));
	ASSERT_TRUE(LoadLines(kDataDir + "SemanticCli3WayRight.cpp", right));

	ThreeWayFixture fx(left, middle, right);
	auto analyzer = fx.MakeAnalyzer();

	const int lineCount = static_cast<int>(left.size());
	SemanticMerge::Suggestion suggestion;
	String message;
	// Left == Right, Middle changed: destination must be a pane that still
	// has the unchanged text.
	const bool ok = analyzer.TryBuildSuggestion(0, MakeDiff(0, lineCount - 1), suggestion, message);
	ASSERT_TRUE(ok) << ucr::toUTF8(message);
	EXPECT_EQ(1, suggestion.changedPane);
}

} // namespace
