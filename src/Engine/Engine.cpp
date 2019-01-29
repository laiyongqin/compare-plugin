/*
 * This file is part of Compare plugin for Notepad++
 * Copyright (C)2011 Jean-Sebastien Leroy (jean.sebastien.leroy@gmail.com)
 * Copyright (C)2017-2018 Pavel Nedev (pg.nedev@gmail.com)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <climits>
#include <exception>
#include <cstdint>
#include <utility>
#include <unordered_set>
#include <set>
#include <unordered_map>
#include <map>
#include <algorithm>

#include <windows.h>

#include "Engine.h"
#include "diff.h"
#include "ProgressDlg.h"


namespace {

enum class charType
{
	SPACECHAR,
	ALPHANUMCHAR,
	OTHERCHAR
};


struct DocCmpInfo
{
	int			view;
	section_t	section;

	int			blockDiffMask;

	std::unordered_set<int>	nonUniqueLines;
};


struct diffLine
{
	diffLine(int lineNum) : line(lineNum) {}

	int line;
	std::vector<section_t> changes;
};


struct blockDiffInfo
{
	const diff_info<blockDiffInfo>*	matchBlock {nullptr};

	std::vector<diffLine>	changedLines;
	std::vector<section_t>	moves;

	inline int movedSection(int line) const
	{
		for (const auto& move: moves)
		{
			if (line >= move.off && line < move.off + move.len)
				return move.len;
		}

		return 0;
	}

	inline bool getNextUnmoved(int& line) const
	{
		for (const auto& move: moves)
		{
			if (line >= move.off && line < move.off + move.len)
			{
				line = move.off + move.len;
				return true;
			}
		}

		return false;
	}
};


using diffInfo = diff_info<blockDiffInfo>;


struct CompareInfo
{
	// Input data
	DocCmpInfo				doc1;
	DocCmpInfo				doc2;

	bool					selectionCompare;

	// Output data - filled by the compare engine
	std::vector<diffInfo>	blockDiffs;
};


struct MatchInfo
{
	int			lookupOff;
	diffInfo*	matchDiff;
	int			matchOff;
	int			matchLen;
};


struct Char
{
	Char(char c, int p) : ch(c), pos(p) {}

	char ch;
	int pos;

	inline bool operator==(const Char& rhs) const
	{
		return (ch == rhs.ch);
	}

	inline bool operator!=(const Char& rhs) const
	{
		return (ch != rhs.ch);
	}

	inline bool operator==(char rhs) const
	{
		return (ch == rhs);
	}

	inline bool operator!=(char rhs) const
	{
		return (ch != rhs);
	}
};


struct Word
{
	int pos;
	int len;

	uint64_t hash;

	inline bool operator==(const Word& rhs) const
	{
		return (hash == rhs.hash);
	}

	inline bool operator!=(const Word& rhs) const
	{
		return (hash != rhs.hash);
	}

	inline bool operator==(uint64_t rhs) const
	{
		return (hash == rhs);
	}

	inline bool operator!=(uint64_t rhs) const
	{
		return (hash != rhs);
	}
};


const uint64_t cHashSeed = 0x84222325;

inline uint64_t Hash(uint64_t hval, char letter)
{
	hval ^= static_cast<uint64_t>(letter);

	hval += (hval << 1) + (hval << 4) + (hval << 5) + (hval << 7) + (hval << 8) + (hval << 40);

	return hval;
}


std::vector<uint64_t> computeLineHashes(DocCmpInfo& doc, const CompareOptions& options)
{
	const int monitorCancelEveryXLine = 500;

	progress_ptr& progress = ProgressDlg::Get();

	int lineCount = CallScintilla(doc.view, SCI_GETLENGTH, 0, 0);

	if (lineCount)
		lineCount = CallScintilla(doc.view, SCI_GETLINECOUNT, 0, 0);
	else
		return std::vector<uint64_t>{};

	if ((doc.section.len <= 0) || (doc.section.off + doc.section.len > lineCount))
		doc.section.len = lineCount - doc.section.off;

	if (progress)
		progress->SetMaxCount((doc.section.len / monitorCancelEveryXLine) + 1);

	std::vector<uint64_t> lineHashes(doc.section.len, cHashSeed);

	for (int lineNum = 0; lineNum < doc.section.len; ++lineNum)
	{
		if (progress && (lineNum % monitorCancelEveryXLine == 0) && !progress->Advance())
			return std::vector<uint64_t>{};

		const int lineStart = CallScintilla(doc.view, SCI_POSITIONFROMLINE, lineNum + doc.section.off, 0);
		const int lineEnd = CallScintilla(doc.view, SCI_GETLINEENDPOSITION, lineNum + doc.section.off, 0);

		if (lineEnd - lineStart)
		{
			std::vector<char> line = getText(doc.view, lineStart, lineEnd);
			const int lineLen = static_cast<int>(line.size()) - 1;

			if (options.ignoreCase)
				toLowerCase(line);

			for (int i = 0; i < lineLen; ++i)
			{
				if (options.ignoreSpaces && (line[i] == ' ' || line[i] == '\t'))
					continue;

				lineHashes[lineNum] = Hash(lineHashes[lineNum], line[i]);
			}
		}
	}

	if (doc.section.len && lineHashes.back() == cHashSeed)
	{
		lineHashes.pop_back();
		--doc.section.len;
	}

	return lineHashes;
}


charType getCharType(char letter)
{
	if (letter == ' ' || letter == '\t')
		return charType::SPACECHAR;

	if (::IsCharAlphaNumericA(letter) || letter == '_')
		return charType::ALPHANUMCHAR;

	return charType::OTHERCHAR;
}


std::vector<Char> getSectionChars(int view, int secStart, int secEnd, const CompareOptions& options)
{
	std::vector<Char> chars;

	if (secEnd - secStart)
	{
		std::vector<char> line = getText(view, secStart, secEnd);
		const int lineLen = static_cast<int>(line.size()) - 1;

		if (options.ignoreCase)
			toLowerCase(line);

		for (int i = 0; i < lineLen; ++i)
		{
			if (!options.ignoreSpaces || getCharType(line[i]) != charType::SPACECHAR)
				chars.emplace_back(line[i], i);
		}
	}

	return chars;
}


std::vector<std::vector<Char>> getChars(int view, int lineOffset, int lineCount, const CompareOptions& options)
{
	std::vector<std::vector<Char>> chars(lineCount);

	for (int lineNum = 0; lineNum < lineCount; ++lineNum)
	{
		const int docLineNum = lineNum + lineOffset;
		const int docLineStart = CallScintilla(view, SCI_POSITIONFROMLINE, docLineNum, 0);
		const int docLineEnd = CallScintilla(view, SCI_GETLINEENDPOSITION, docLineNum, 0);

		if (docLineEnd - docLineStart)
			chars[lineNum] = getSectionChars(view, docLineStart, docLineEnd, options);
	}

	return chars;
}


std::vector<Word> getLineWords(int view, int lineNum, const CompareOptions& options)
{
	std::vector<Word> words;

	const int docLineStart = CallScintilla(view, SCI_POSITIONFROMLINE, lineNum, 0);
	const int docLineEnd = CallScintilla(view, SCI_GETLINEENDPOSITION, lineNum, 0);

	if (docLineEnd - docLineStart)
	{
		std::vector<char> line = getText(view, docLineStart, docLineEnd);
		const int lineLen = static_cast<int>(line.size()) - 1;

		if (options.ignoreCase)
			toLowerCase(line);

		charType currentWordType = getCharType(line[0]);

		Word word;
		word.hash = Hash(cHashSeed, line[0]);
		word.pos = 0;
		word.len = 1;

		for (int i = 1; i < lineLen; ++i)
		{
			charType newWordType = getCharType(line[i]);

			if (newWordType == currentWordType)
			{
				++word.len;
				word.hash = Hash(word.hash, line[i]);
			}
			else
			{
				if (!options.ignoreSpaces || currentWordType != charType::SPACECHAR)
					words.push_back(word);

				currentWordType = newWordType;

				word.hash = Hash(cHashSeed, line[i]);
				word.pos = i;
				word.len = 1;
			}
		}

		if (!options.ignoreSpaces || currentWordType != charType::SPACECHAR)
			words.push_back(word);
	}

	return words;
}


// Scan for the best single matching block in the other file
void findBestMatch(const CompareInfo& cmpInfo,
		const std::vector<uint64_t>& lineHashes1, const std::vector<uint64_t>& lineHashes2,
		const diffInfo& lookupDiff, int lookupOff, MatchInfo& mi)
{
	mi.matchLen		= 0;
	mi.matchDiff	= nullptr;

	const std::vector<uint64_t>* pLookupLines;
	const std::vector<uint64_t>* pMatchLines;
	diff_type matchType;

	if (lookupDiff.type == diff_type::DIFF_IN_1)
	{
		pLookupLines	= &lineHashes1;
		pMatchLines		= &lineHashes2;
		matchType		= diff_type::DIFF_IN_2;
	}
	else
	{
		pLookupLines	= &lineHashes2;
		pMatchLines		= &lineHashes1;
		matchType		= diff_type::DIFF_IN_1;
	}

	int minMatchLen = 1;

	for (const diffInfo& matchDiff: cmpInfo.blockDiffs)
	{
		if (matchDiff.type != matchType || matchDiff.len < minMatchLen)
			continue;

		int matchLastUnmoved = 0;

		for (int matchOff = 0; matchOff < matchDiff.len; ++matchOff)
		{
			if ((*pLookupLines)[lookupDiff.off + lookupOff] != (*pMatchLines)[matchDiff.off + matchOff])
				continue;

			if (matchDiff.info.getNextUnmoved(matchOff))
			{
				matchLastUnmoved = matchOff;
				--matchOff;
				continue;
			}

			int lookupStart	= lookupOff - 1;
			int matchStart	= matchOff - 1;

			// Check for the beginning of the matched block (containing lookupOff element)
			for (; lookupStart >= 0 && matchStart >= matchLastUnmoved &&
					(*pLookupLines)[lookupDiff.off + lookupStart] == (*pMatchLines)[matchDiff.off + matchStart] &&
					!lookupDiff.info.movedSection(lookupStart);
					--lookupStart, --matchStart);

			++lookupStart;
			++matchStart;

			int lookupEnd	= lookupOff + 1;
			int matchEnd	= matchOff + 1;

			// Check for the end of the matched block (containing lookupOff element)
			for (; lookupEnd < lookupDiff.len && matchEnd < matchDiff.len &&
					(*pLookupLines)[lookupDiff.off + lookupEnd] == (*pMatchLines)[matchDiff.off + matchEnd] &&
					!lookupDiff.info.movedSection(lookupEnd) && !matchDiff.info.movedSection(matchEnd);
					++lookupEnd, ++matchEnd);

			const int matchLen = lookupEnd - lookupStart;

			if (mi.matchLen < matchLen)
			{
				mi.lookupOff	= lookupStart;
				mi.matchDiff	= const_cast<diffInfo*>(&matchDiff);
				mi.matchOff		= matchStart;
				mi.matchLen		= matchLen;

				minMatchLen		= matchLen;
			}
			else if (mi.matchLen == matchLen)
			{
				mi.matchDiff = nullptr;
			}
		}
	}
}


// Recursively resolve the best match
bool resolveMatch(const CompareInfo& cmpInfo,
		const std::vector<uint64_t>& lineHashes1, const std::vector<uint64_t>& lineHashes2,
		diffInfo& lookupDiff, int lookupOff, MatchInfo& lookupMi)
{
	bool ret = false;

	if (lookupMi.matchDiff)
	{
		lookupOff = lookupMi.matchOff + (lookupOff - lookupMi.lookupOff);

		MatchInfo reverseMi;
		findBestMatch(cmpInfo, lineHashes1, lineHashes2, *(lookupMi.matchDiff), lookupOff, reverseMi);

		if (reverseMi.matchDiff == &lookupDiff)
		{
			lookupDiff.info.moves.emplace_back(lookupMi.lookupOff, lookupMi.matchLen);
			lookupMi.matchDiff->info.moves.emplace_back(lookupMi.matchOff, lookupMi.matchLen);
			ret = true;
		}
		else if (reverseMi.matchDiff)
		{
			ret = resolveMatch(cmpInfo, lineHashes1, lineHashes2, *(lookupMi.matchDiff), lookupOff, reverseMi);
			lookupMi.matchLen = 0;
		}
	}

	return ret;
}


void findMoves(CompareInfo& cmpInfo,
		const std::vector<uint64_t>& lineHashes1, const std::vector<uint64_t>& lineHashes2)
{
	// LOGD("FIND MOVES\n");

	bool repeat = true;

	while (repeat)
	{
		repeat = false;

		for (diffInfo& lookupDiff: cmpInfo.blockDiffs)
		{
			if (lookupDiff.type != diff_type::DIFF_IN_1)
				continue;

			// LOGD("DIFF_IN_1 offset: " + std::to_string(lookupDiff.off + 1) + "\n");

			// Go through all lookupDiff's elements and check if each is matched
			for (int lookupEi = 0; lookupEi < lookupDiff.len; ++lookupEi)
			{
				// Skip already detected moves
				if (lookupDiff.info.getNextUnmoved(lookupEi))
				{
					--lookupEi;
					continue;
				}

				// LOGD("line offset: " + std::to_string(lookupEi) + "\n");

				MatchInfo mi;
				findBestMatch(cmpInfo, lineHashes1, lineHashes2, lookupDiff, lookupEi, mi);

				if (resolveMatch(cmpInfo, lineHashes1, lineHashes2, lookupDiff, lookupEi, mi))
				{
					repeat = true;

					if (mi.matchLen)
						lookupEi = mi.lookupOff + mi.matchLen - 1;
					else
						--lookupEi;

					// LOGD("move match found, next line offset: " + std::to_string(lookupEi + 1) + "\n");
				}
			}
		}
	}
}


void findUniqueLines(CompareInfo& cmpInfo,
		const std::vector<uint64_t>& lineHashes1, const std::vector<uint64_t>& lineHashes2)
{
	std::unordered_map<uint64_t, std::vector<int>> doc1LinesMap;

	int sectionEnd = cmpInfo.doc1.section.off + cmpInfo.doc1.section.len;
	if (sectionEnd > static_cast<int>(lineHashes1.size()))
		sectionEnd = static_cast<int>(lineHashes1.size());

	for (int i = cmpInfo.doc1.section.off; i < sectionEnd; ++i)
	{
		auto insertPair = doc1LinesMap.emplace(lineHashes1[i], std::vector<int>{i});
		if (!insertPair.second)
			insertPair.first->second.emplace_back(i);
	}

	sectionEnd = cmpInfo.doc2.section.off + cmpInfo.doc2.section.len;
	if (sectionEnd > static_cast<int>(lineHashes2.size()))
		sectionEnd = static_cast<int>(lineHashes2.size());

	for (int i = cmpInfo.doc2.section.off; i < sectionEnd; ++i)
	{
		std::unordered_map<uint64_t, std::vector<int>>::iterator doc1it = doc1LinesMap.find(lineHashes2[i]);

		if (doc1it != doc1LinesMap.end())
		{
			cmpInfo.doc2.nonUniqueLines.emplace(i);

			auto insertPair = cmpInfo.doc1.nonUniqueLines.emplace(doc1it->second[0]);
			if (insertPair.second)
			{
				for (unsigned int j = 1; j < doc1it->second.size(); ++j)
					cmpInfo.doc1.nonUniqueLines.emplace(doc1it->second[j]);
			}
		}
	}
}


void compareLines(const DocCmpInfo& doc1, const DocCmpInfo& doc2, diffInfo& blockDiff1, diffInfo& blockDiff2,
		const std::map<int, std::pair<float, int>>& lineMappings, const CompareOptions& options)
{
	int lastLine2 = -1;

	for (const auto& lm: lineMappings)
	{
		// lines1 are stored in ascending order and to have a match lines2 must also be in ascending order
		if (lm.second.second <= lastLine2)
			continue;

		int line1 = lm.first;
		int line2 = lm.second.second;

		LOGD("CompareLines " + std::to_string(blockDiff1.off + line1 + 1) + " and " +
				std::to_string(blockDiff2.off + line2 + 1) + "\n");

		lastLine2 = line2;

		const std::vector<Word> lineWords1 = getLineWords(doc1.view, blockDiff1.off + line1, options);
		const std::vector<Word> lineWords2 = getLineWords(doc2.view, blockDiff2.off + line2, options);

		const auto* pLine1 = &lineWords1;
		const auto* pLine2 = &lineWords2;

		const DocCmpInfo* pDoc1 = &doc1;
		const DocCmpInfo* pDoc2 = &doc2;

		diffInfo* pBlockDiff1 = &blockDiff1;
		diffInfo* pBlockDiff2 = &blockDiff2;

		if (pLine1->size() < pLine2->size())
		{
			std::swap(pDoc1, pDoc2);
			std::swap(pBlockDiff1, pBlockDiff2);
			std::swap(pLine1, pLine2);
			std::swap(line1, line2);
		}

		// First use word granularity (find matching words) for better precision
		const std::vector<diff_info<void>> lineDiffs = DiffCalc<Word>(*pLine1, *pLine2)();
		const int lineDiffsSize = static_cast<int>(lineDiffs.size());

		pBlockDiff1->info.changedLines.emplace_back(line1);
		pBlockDiff2->info.changedLines.emplace_back(line2);

		const int lineOff1 = CallScintilla(pDoc1->view, SCI_POSITIONFROMLINE, line1 + pBlockDiff1->off, 0);
		const int lineOff2 = CallScintilla(pDoc2->view, SCI_POSITIONFROMLINE, line2 + pBlockDiff2->off, 0);

		int lineLen1 = 0;
		int lineLen2 = 0;

		for (const auto& word: *pLine1)
			lineLen1 += word.len;

		for (const auto& word: *pLine2)
			lineLen2 += word.len;

		int totalLineMatchLen = 0;

		for (int i = 0; i < lineDiffsSize; ++i)
		{
			const auto& ld = lineDiffs[i];

			if (ld.type == diff_type::DIFF_IN_1)
			{
				// Resolve words mismatched DIFF_IN_1 / DIFF_IN_2 pairs to find possible sub-word similarities
				if (options.charPrecision &&
					(i + 1 < lineDiffsSize) && (lineDiffs[i + 1].type == diff_type::DIFF_IN_2))
				{
					const auto& ld2 = lineDiffs[i + 1];

					int off1 = (*pLine1)[ld.off].pos;
					int end1 = (*pLine1)[ld.off + ld.len - 1].pos + (*pLine1)[ld.off + ld.len - 1].len;

					int off2 = (*pLine2)[ld2.off].pos;
					int end2 = (*pLine2)[ld2.off + ld2.len - 1].pos + (*pLine2)[ld2.off + ld2.len - 1].len;

					const std::vector<Char> sec1 =
							getSectionChars(pDoc1->view, off1 + lineOff1, end1 + lineOff1, options);
					const std::vector<Char> sec2 =
							getSectionChars(pDoc2->view, off2 + lineOff2, end2 + lineOff2, options);

					LOGD("Compare sections " +
							std::to_string(off1 + lineOff1 + 1) + " to " +
							std::to_string(end1 + lineOff1 + 1) + " and " +
							std::to_string(off2 + lineOff2 + 1) + " to " +
							std::to_string(end2 + lineOff2 + 1) + "\n");

					const auto* pSec1 = &sec1;
					const auto* pSec2 = &sec2;

					diffInfo* pBD1 = pBlockDiff1;
					diffInfo* pBD2 = pBlockDiff2;

					if (pSec1->size() < pSec2->size())
					{
						std::swap(pSec1, pSec2);
						std::swap(pBD1, pBD2);
						std::swap(off1, off2);
						std::swap(end1, end2);
					}

					// Compare changed words sections
					const std::vector<diff_info<void>> sectionDiffs = DiffCalc<Char>(*pSec1, *pSec2)();

					int matchLen = 0;
					int matchSections = 0;

					for (const auto& sd: sectionDiffs)
					{
						if (sd.type == diff_type::DIFF_MATCH)
						{
							matchLen += sd.len;
							++matchSections;
						}
					}

					if (matchSections)
					{
						LOGD("Matching sections found: " +
								std::to_string(matchSections) + ", matched len: " + std::to_string(matchLen) + "\n");

						// Are similarities a considerable portion of the diff?
						if ((int)((matchLen * 100) / pSec1->size()) >= options.matchPercentThreshold)
						{
							for (const auto& sd: sectionDiffs)
							{
								if (sd.type == diff_type::DIFF_IN_1)
								{
									section_t change;

									change.off = (*pSec1)[sd.off].pos + off1;
									change.len = (*pSec1)[sd.off + sd.len - 1].pos + off1 + 1 - change.off;

									pBD1->info.changedLines.back().changes.emplace_back(change);
								}
								else if (sd.type == diff_type::DIFF_IN_2)
								{
									section_t change;

									change.off = (*pSec2)[sd.off].pos + off2;
									change.len = (*pSec2)[sd.off + sd.len - 1].pos + off2 + 1 - change.off;

									pBD2->info.changedLines.back().changes.emplace_back(change);
								}
							}

							totalLineMatchLen += matchLen;

							LOGD("And marked as a whole\n");

							++i;
							continue;
						}
						// If not, mark only beginning and ending diff section matches
						else
						{
							int startMatch = 0;
							while ((*pSec1)[startMatch] == (*pSec2)[startMatch])
								++startMatch;

							int endMatch = 0;
							while (((int)pSec2->size() - endMatch - 1 > startMatch) &&
									((*pSec1)[pSec1->size() - endMatch - 1] == (*pSec2)[pSec2->size() - endMatch - 1]))
								++endMatch;

							// Always match characters in the beginning and at the end
							if (startMatch || endMatch)
							{
								section_t change;

								change.off = off1;
								if (startMatch)
									change.off += (*pSec1)[startMatch - 1].pos + 1;

								change.len = (endMatch ?
										(*pSec1)[pSec1->size() - endMatch - 1].pos + 1 + off1 : end1) - change.off;

								if (change.len > 0)
									pBD1->info.changedLines.back().changes.emplace_back(change);

								change.off = off2;
								if (startMatch)
									change.off += (*pSec2)[startMatch - 1].pos + 1;

								change.len = (endMatch ?
										(*pSec2)[pSec2->size() - endMatch - 1].pos + 1 + off2 : end2) - change.off;

								if (change.len > 0)
									pBD2->info.changedLines.back().changes.emplace_back(change);

								totalLineMatchLen += startMatch + endMatch;

								LOGD("And marked in the beginning and at the end\n");

								++i;
								continue;
							}
						}
					}

					// No matching sections between the lines found - move to next lines
					if (lineDiffsSize == 2)
						break;
				}

				section_t change;

				change.off = (*pLine1)[ld.off].pos;
				change.len = (*pLine1)[ld.off + ld.len - 1].pos + (*pLine1)[ld.off + ld.len - 1].len - change.off;

				pBlockDiff1->info.changedLines.back().changes.emplace_back(change);
			}
			else if (ld.type == diff_type::DIFF_IN_2)
			{
				section_t change;

				change.off = (*pLine2)[ld.off].pos;
				change.len = (*pLine2)[ld.off + ld.len - 1].pos + (*pLine2)[ld.off + ld.len - 1].len - change.off;

				pBlockDiff2->info.changedLines.back().changes.emplace_back(change);
			}
			else
			{
				for (int j = 0; j < ld.len; ++j)
					totalLineMatchLen += (*pLine1)[ld.off + j].len;
			}
		}

		// Not enough portion of the lines matches - consider them totally different
		if (((totalLineMatchLen * 100) / (lineLen1 > lineLen2 ? lineLen1 : lineLen2)) < options.matchPercentThreshold)
		{
			pBlockDiff1->info.changedLines.pop_back();
			pBlockDiff2->info.changedLines.pop_back();
		}
	}
}


void compareBlocks(const DocCmpInfo& doc1, const DocCmpInfo& doc2, diffInfo& blockDiff1, diffInfo& blockDiff2,
		const CompareOptions& options)
{
	const std::vector<std::vector<Char>> chunk1 = getChars(doc1.view, blockDiff1.off, blockDiff1.len, options);
	const std::vector<std::vector<Char>> chunk2 = getChars(doc2.view, blockDiff2.off, blockDiff2.len, options);

	const int linesCount1 = static_cast<int>(chunk1.size());
	const int linesCount2 = static_cast<int>(chunk2.size());

	struct conv_key
	{
		float convergence;
		int line1;
		int line2;

		conv_key(float c, int l1, int l2) : convergence(c), line1(l1), line2(l2)
		{}

		bool operator<(const conv_key& rhs) const
		{
			return ((convergence > rhs.convergence) ||
					((convergence == rhs.convergence) && ((line1 < rhs.line1) ||
						((line1 == rhs.line1) && ((line2 < rhs.line2))))));
		}
	};

	std::set<conv_key> orderedLinesConvergence;

	for (int line1 = 0; line1 < linesCount1; ++line1)
	{
		if (chunk1[line1].empty())
			continue;

		if (blockDiff1.info.getNextUnmoved(line1))
		{
			--line1;
			continue;
		}

		for (int line2 = 0; line2 < linesCount2; ++line2)
		{
			if (chunk2[line2].empty())
				continue;

			if (blockDiff2.info.getNextUnmoved(line2))
			{
				--line2;
				continue;
			}

			const auto* pLine1 = &chunk1[line1];
			const auto* pLine2 = &chunk2[line2];

			if (pLine1->size() < pLine2->size())
				std::swap(pLine1, pLine2);

			if ((int)((pLine2->size() * 100) / pLine1->size()) < options.matchPercentThreshold)
				continue;

			const std::vector<diff_info<void>> lineDiffs = DiffCalc<Char>(*pLine1, *pLine2)();

			float lineConvergence = 0;

			for (const auto& ld: lineDiffs)
			{
				if (ld.type == diff_type::DIFF_MATCH)
					lineConvergence += ld.len;
			}

			lineConvergence = lineConvergence * 100 / pLine1->size();

			if (lineConvergence >= options.matchPercentThreshold)
				orderedLinesConvergence.emplace(conv_key(lineConvergence, line1, line2));
		}
	}

	std::map<int, std::pair<float, int>> bestLineMappings;
	float bestBlockConvergence = 0;

	for (auto startItr = orderedLinesConvergence.begin(); startItr != orderedLinesConvergence.end(); ++startItr)
	{
		std::map<int, std::pair<float, int>> lineMappings;

		std::vector<bool> mappedLines1(linesCount1, false);
		std::vector<bool> mappedLines2(linesCount2, false);

		int mappedLinesCount1 = 0;
		int mappedLinesCount2 = 0;

		for (auto ocItr = startItr; ocItr != orderedLinesConvergence.end(); ++ocItr)
		{
			if (!mappedLines1[ocItr->line1] && !mappedLines2[ocItr->line2])
			{
				lineMappings.emplace(ocItr->line1, std::pair<float, int>(ocItr->convergence, ocItr->line2));

				if ((++mappedLinesCount1 == linesCount1) || (++mappedLinesCount2 == linesCount2))
					break;

				mappedLines1[ocItr->line1] = true;
				mappedLines2[ocItr->line2] = true;
			}
		}

		float currentBlockConvergence = 0;
		int lastLine2 = -1;

		for (const auto& lm: lineMappings)
		{
			// lines1 are stored in ascending order and to have a match lines2 must also be in ascending order
			if (lm.second.second > lastLine2)
			{
				currentBlockConvergence += lm.second.first;
				lastLine2 = lm.second.second;
			}
		}

		if (bestBlockConvergence < currentBlockConvergence)
		{
			bestBlockConvergence = currentBlockConvergence;
			bestLineMappings = std::move(lineMappings);
		}
	}

	if (!bestLineMappings.empty())
		compareLines(doc1, doc2, blockDiff1, blockDiff2, bestLineMappings, options);

	return;
}


void markSection(const diffInfo& bd, const DocCmpInfo& doc)
{
	const int endOff = doc.section.off + doc.section.len;

	for (int i = doc.section.off, line = bd.off + doc.section.off; i < endOff; ++i, ++line)
	{
		int movedLen = bd.info.movedSection(i);

		if (movedLen > doc.section.len)
			movedLen = doc.section.len;

		if (movedLen == 0)
		{
			int mark = doc.blockDiffMask;

			if (doc.nonUniqueLines.find(line) != doc.nonUniqueLines.end())
				mark = (mark == MARKER_MASK_ADDED) ? MARKER_MASK_ADDED_LOCAL : MARKER_MASK_REMOVED_LOCAL;

			CallScintilla(doc.view, SCI_MARKERADDSET, line, mark);
		}
		else if (movedLen == 1)
		{
			CallScintilla(doc.view, SCI_MARKERADDSET, line, MARKER_MASK_MOVED_LINE);
		}
		else
		{
			i += --movedLen;

			CallScintilla(doc.view, SCI_MARKERADDSET, line, MARKER_MASK_MOVED_BEGIN);

			for (--movedLen, ++line; movedLen; --movedLen, ++line)
				CallScintilla(doc.view, SCI_MARKERADDSET, line, MARKER_MASK_MOVED_MID);

			CallScintilla(doc.view, SCI_MARKERADDSET, line, MARKER_MASK_MOVED_END);
		}
	}
}


void markLineDiffs(const CompareInfo& cmpInfo, const diffInfo& bd, int lineIdx)
{
	int line = bd.off + bd.info.changedLines[lineIdx].line;
	int linePos = CallScintilla(cmpInfo.doc1.view, SCI_POSITIONFROMLINE, line, 0);

	for (const auto& change: bd.info.changedLines[lineIdx].changes)
		markTextAsChanged(cmpInfo.doc1.view, linePos + change.off, change.len);

	CallScintilla(cmpInfo.doc1.view, SCI_MARKERADDSET, line,
			cmpInfo.doc1.nonUniqueLines.find(line) == cmpInfo.doc1.nonUniqueLines.end() ?
			MARKER_MASK_CHANGED : MARKER_MASK_CHANGED_LOCAL);

	line = bd.info.matchBlock->off + bd.info.matchBlock->info.changedLines[lineIdx].line;
	linePos = CallScintilla(cmpInfo.doc2.view, SCI_POSITIONFROMLINE, line, 0);

	for (const auto& change: bd.info.matchBlock->info.changedLines[lineIdx].changes)
		markTextAsChanged(cmpInfo.doc2.view, linePos + change.off, change.len);

	CallScintilla(cmpInfo.doc2.view, SCI_MARKERADDSET, line,
			cmpInfo.doc2.nonUniqueLines.find(line) == cmpInfo.doc2.nonUniqueLines.end() ?
			MARKER_MASK_CHANGED : MARKER_MASK_CHANGED_LOCAL);
}


bool markAllDiffs(CompareInfo& cmpInfo, AlignmentInfo_t& alignmentInfo)
{
	progress_ptr& progress = ProgressDlg::Get();

	alignmentInfo.clear();

	const int blockDiffSize = static_cast<int>(cmpInfo.blockDiffs.size());

	if (progress)
		progress->SetMaxCount(blockDiffSize);

	AlignmentPair alignPair;

	AlignmentViewData* pMainAlignData	= &alignPair.main;
	AlignmentViewData* pSubAlignData	= &alignPair.sub;

	// Make sure pMainAlignData is linked to doc1
	if (cmpInfo.doc1.view == SUB_VIEW)
		std::swap(pMainAlignData, pSubAlignData);

	pMainAlignData->line	= cmpInfo.doc1.section.off;
	pSubAlignData->line		= cmpInfo.doc2.section.off;

	for (int i = 0; i < blockDiffSize; ++i)
	{
		const diffInfo& bd = cmpInfo.blockDiffs[i];

		if (bd.type == diff_type::DIFF_MATCH)
		{
			pMainAlignData->diffMask	= 0;
			pSubAlignData->diffMask		= 0;

			alignmentInfo.push_back(alignPair);

			pMainAlignData->line	+= bd.len;
			pSubAlignData->line		+= bd.len;
		}
		else if (bd.type == diff_type::DIFF_IN_2)
		{
			cmpInfo.doc2.section.off = 0;
			cmpInfo.doc2.section.len = bd.len;
			markSection(bd, cmpInfo.doc2);

			pMainAlignData->diffMask	= 0;
			pSubAlignData->diffMask		= cmpInfo.doc2.blockDiffMask;

			alignmentInfo.push_back(alignPair);

			pSubAlignData->line += bd.len;
		}
		else if (bd.type == diff_type::DIFF_IN_1)
		{
			if (bd.info.matchBlock)
			{
				const int changedLinesCount = static_cast<int>(bd.info.changedLines.size());

				cmpInfo.doc1.section.off = 0;
				cmpInfo.doc2.section.off = 0;

				for (int j = 0; j < changedLinesCount; ++j)
				{
					cmpInfo.doc1.section.len = bd.info.changedLines[j].line - cmpInfo.doc1.section.off;
					cmpInfo.doc2.section.len = bd.info.matchBlock->info.changedLines[j].line - cmpInfo.doc2.section.off;

					if (cmpInfo.doc1.section.len || cmpInfo.doc2.section.len)
					{
						pMainAlignData->diffMask	= cmpInfo.doc1.section.len ? cmpInfo.doc1.blockDiffMask : 0;
						pSubAlignData->diffMask		= cmpInfo.doc2.section.len ? cmpInfo.doc2.blockDiffMask : 0;

						alignmentInfo.push_back(alignPair);

						if (cmpInfo.doc1.section.len)
						{
							markSection(bd, cmpInfo.doc1);
							pMainAlignData->line += cmpInfo.doc1.section.len;
						}

						if (cmpInfo.doc2.section.len)
						{
							markSection(*bd.info.matchBlock, cmpInfo.doc2);
							pSubAlignData->line += cmpInfo.doc2.section.len;
						}
					}

					pMainAlignData->diffMask	= MARKER_MASK_CHANGED;
					pSubAlignData->diffMask		= MARKER_MASK_CHANGED;

					alignmentInfo.push_back(alignPair);

					markLineDiffs(cmpInfo, bd, j);

					cmpInfo.doc1.section.off = bd.info.changedLines[j].line + 1;
					cmpInfo.doc2.section.off = bd.info.matchBlock->info.changedLines[j].line + 1;

					++(pMainAlignData->line);
					++(pSubAlignData->line);
				}

				cmpInfo.doc1.section.len = bd.len - cmpInfo.doc1.section.off;
				cmpInfo.doc2.section.len = bd.info.matchBlock->len - cmpInfo.doc2.section.off;

				if (cmpInfo.doc1.section.len || cmpInfo.doc2.section.len)
				{
					pMainAlignData->diffMask	= cmpInfo.doc1.section.len ? cmpInfo.doc1.blockDiffMask : 0;
					pSubAlignData->diffMask		= cmpInfo.doc2.section.len ? cmpInfo.doc2.blockDiffMask : 0;

					alignmentInfo.push_back(alignPair);

					if (cmpInfo.doc1.section.len)
					{
						markSection(bd, cmpInfo.doc1);
						pMainAlignData->line += cmpInfo.doc1.section.len;
					}

					if (cmpInfo.doc2.section.len)
					{
						markSection(*bd.info.matchBlock, cmpInfo.doc2);
						pSubAlignData->line += cmpInfo.doc2.section.len;
					}
				}

				++i;
			}
			else
			{
				cmpInfo.doc1.section.off = 0;
				cmpInfo.doc1.section.len = bd.len;
				markSection(bd, cmpInfo.doc1);

				pMainAlignData->diffMask	= cmpInfo.doc1.blockDiffMask;
				pSubAlignData->diffMask		= 0;

				alignmentInfo.push_back(alignPair);

				pMainAlignData->line += bd.len;
			}
		}

		if (progress && !progress->Advance())
			return false;
	}

	if (cmpInfo.selectionCompare)
	{
		pMainAlignData->diffMask	= 0;
		pSubAlignData->diffMask		= 0;

		alignmentInfo.push_back(alignPair);
	}

	if (progress && !progress->NextPhase())
		return false;

	return true;
}


CompareResult runCompare(const CompareOptions& options, AlignmentInfo_t& alignmentInfo)
{
	progress_ptr& progress = ProgressDlg::Get();

	CompareInfo cmpInfo;

	cmpInfo.doc1.view	= MAIN_VIEW;
	cmpInfo.doc2.view	= SUB_VIEW;

	cmpInfo.selectionCompare	= options.selectionCompare;

	if (options.selectionCompare)
	{
		cmpInfo.doc1.section.off	= options.selections[MAIN_VIEW].first;
		cmpInfo.doc1.section.len	= options.selections[MAIN_VIEW].second - options.selections[MAIN_VIEW].first + 1;

		cmpInfo.doc2.section.off	= options.selections[SUB_VIEW].first;
		cmpInfo.doc2.section.len	= options.selections[SUB_VIEW].second - options.selections[SUB_VIEW].first + 1;
	}

	cmpInfo.doc1.blockDiffMask = (options.oldFileViewId == MAIN_VIEW) ? MARKER_MASK_REMOVED : MARKER_MASK_ADDED;
	cmpInfo.doc2.blockDiffMask = (options.oldFileViewId == MAIN_VIEW) ? MARKER_MASK_ADDED : MARKER_MASK_REMOVED;

	const std::vector<uint64_t> doc1LineHashes = computeLineHashes(cmpInfo.doc1, options);

	if (progress && !progress->NextPhase())
		return CompareResult::COMPARE_CANCELLED;

	const std::vector<uint64_t> doc2LineHashes = computeLineHashes(cmpInfo.doc2, options);

	if (progress && !progress->NextPhase())
		return CompareResult::COMPARE_CANCELLED;

	const std::vector<uint64_t>* pLineHashes1 = &doc1LineHashes;
	const std::vector<uint64_t>* pLineHashes2 = &doc2LineHashes;

	if (pLineHashes1->size() < pLineHashes2->size())
	{
		std::swap(pLineHashes1, pLineHashes2);
		std::swap(cmpInfo.doc1, cmpInfo.doc2);
	}

	cmpInfo.blockDiffs = DiffCalc<uint64_t, blockDiffInfo>(*pLineHashes1, *pLineHashes2)();

	const int blockDiffsSize = static_cast<int>(cmpInfo.blockDiffs.size());

	if (blockDiffsSize == 0 || (blockDiffsSize == 1 && cmpInfo.blockDiffs[0].type == diff_type::DIFF_MATCH))
		return CompareResult::COMPARE_MATCH;

	findUniqueLines(cmpInfo, *pLineHashes1, *pLineHashes2);

	if (options.detectMoves)
		findMoves(cmpInfo, *pLineHashes1, *pLineHashes2);

	if (cmpInfo.doc1.section.off || cmpInfo.doc2.section.off)
	{
		for (auto& bd: cmpInfo.blockDiffs)
		{
			if (bd.type == diff_type::DIFF_IN_1 || bd.type == diff_type::DIFF_MATCH)
				bd.off += cmpInfo.doc1.section.off;
			else if (bd.type == diff_type::DIFF_IN_2)
				bd.off += cmpInfo.doc2.section.off;
		}
	}

	if (progress && !progress->NextPhase())
		return CompareResult::COMPARE_CANCELLED;

	if (progress)
		progress->SetMaxCount(blockDiffsSize);

	// Do block compares
	for (int i = 0; i < blockDiffsSize; ++i)
	{
		if (cmpInfo.blockDiffs[i].type == diff_type::DIFF_IN_2)
		{
			// Check if the DIFF_IN_1 / DIFF_IN_2 pair includes changed lines or it's a completely replaced block
			if (i != 0 && cmpInfo.blockDiffs[i - 1].type == diff_type::DIFF_IN_1)
			{
				diffInfo& blockDiff1 = cmpInfo.blockDiffs[i - 1];
				diffInfo& blockDiff2 = cmpInfo.blockDiffs[i];

				blockDiff1.info.matchBlock = &blockDiff2;
				blockDiff2.info.matchBlock = &blockDiff1;

				compareBlocks(cmpInfo.doc1, cmpInfo.doc2, blockDiff1, blockDiff2, options);
			}
		}

		if (progress && !progress->Advance())
			return CompareResult::COMPARE_CANCELLED;
	}

	if (progress && !progress->NextPhase())
		return CompareResult::COMPARE_CANCELLED;

	if (!markAllDiffs(cmpInfo, alignmentInfo))
		return CompareResult::COMPARE_CANCELLED;

	return CompareResult::COMPARE_MISMATCH;
}


CompareResult runFindUnique(const CompareOptions& options, AlignmentInfo_t& alignmentInfo)
{
	progress_ptr& progress = ProgressDlg::Get();

	alignmentInfo.clear();

	DocCmpInfo doc1;
	DocCmpInfo doc2;

	doc1.view	= MAIN_VIEW;
	doc2.view	= SUB_VIEW;

	if (options.selectionCompare)
	{
		doc1.section.off	= options.selections[MAIN_VIEW].first;
		doc1.section.len	= options.selections[MAIN_VIEW].second - options.selections[MAIN_VIEW].first + 1;

		doc2.section.off	= options.selections[SUB_VIEW].first;
		doc2.section.len	= options.selections[SUB_VIEW].second - options.selections[SUB_VIEW].first + 1;
	}

	if (options.oldFileViewId == MAIN_VIEW)
	{
		doc1.blockDiffMask = MARKER_MASK_REMOVED;
		doc2.blockDiffMask = MARKER_MASK_ADDED;
	}
	else
	{
		doc1.blockDiffMask = MARKER_MASK_ADDED;
		doc2.blockDiffMask = MARKER_MASK_REMOVED;
	}

	std::vector<uint64_t> doc1LineHashes = computeLineHashes(doc1, options);

	if (progress && !progress->NextPhase())
		return CompareResult::COMPARE_CANCELLED;

	std::vector<uint64_t> doc2LineHashes = computeLineHashes(doc2, options);

	if (progress && !progress->NextPhase())
		return CompareResult::COMPARE_CANCELLED;

	std::unordered_map<uint64_t, std::vector<int>> doc1UniqueLines;

	int docHashesSize = static_cast<int>(doc1LineHashes.size());

	for (int i = 0; i < docHashesSize; ++i)
	{
		auto insertPair = doc1UniqueLines.emplace(doc1LineHashes[i], std::vector<int>{i});
		if (!insertPair.second)
			insertPair.first->second.emplace_back(i);
	}

	doc1LineHashes.clear();

	if (progress && !progress->NextPhase())
		return CompareResult::COMPARE_CANCELLED;

	std::unordered_map<uint64_t, std::vector<int>> doc2UniqueLines;

	docHashesSize = static_cast<int>(doc2LineHashes.size());

	for (int i = 0; i < docHashesSize; ++i)
	{
		auto insertPair = doc2UniqueLines.emplace(doc2LineHashes[i], std::vector<int>{i});
		if (!insertPair.second)
			insertPair.first->second.emplace_back(i);
	}

	doc2LineHashes.clear();

	if (progress && !progress->NextPhase())
		return CompareResult::COMPARE_CANCELLED;

	int doc1UniqueLinesCount = 0;

	for (std::unordered_map<uint64_t, std::vector<int>>::iterator doc1it = doc1UniqueLines.begin();
		doc1it != doc1UniqueLines.end(); ++doc1it)
	{
		std::unordered_map<uint64_t, std::vector<int>>::iterator doc2it = doc2UniqueLines.find(doc1it->first);

		if (doc2it != doc2UniqueLines.end())
		{
			doc2UniqueLines.erase(doc2it);
		}
		else
		{
			for (const auto& line: doc1it->second)
			{
				CallScintilla(doc1.view, SCI_MARKERADDSET, line + doc1.section.off, doc1.blockDiffMask);
				++doc1UniqueLinesCount;
			}
		}
	}

	if (doc1UniqueLinesCount == 0 && doc2UniqueLines.empty())
		return CompareResult::COMPARE_MATCH;

	for (const auto& uniqueLine: doc2UniqueLines)
	{
		for (const auto& line: uniqueLine.second)
		{
			CallScintilla(doc2.view, SCI_MARKERADDSET, line + doc2.section.off, doc2.blockDiffMask);
		}
	}

	AlignmentPair align;
	align.main.line	= doc1.section.off;
	align.sub.line	= doc2.section.off;

	alignmentInfo.push_back(align);

	return CompareResult::COMPARE_MISMATCH;
}

}


CompareResult compareViews(const CompareOptions& options, const TCHAR* progressInfo, AlignmentInfo_t& alignmentInfo)
{
	CompareResult result = CompareResult::COMPARE_ERROR;

	if (progressInfo)
		ProgressDlg::Open(progressInfo);

	try
	{
		if (options.findUniqueMode)
			result = runFindUnique(options, alignmentInfo);
		else
			result = runCompare(options, alignmentInfo);

		ProgressDlg::Close();
	}
	catch (std::exception& e)
	{
		ProgressDlg::Close();

		char msg[128];
		_snprintf_s(msg, _countof(msg), _TRUNCATE, "Exception occurred: %s", e.what());
		::MessageBoxA(nppData._nppHandle, msg, "Compare", MB_OK | MB_ICONWARNING);
	}
	catch (...)
	{
		ProgressDlg::Close();

		::MessageBoxA(nppData._nppHandle, "Unknown exception occurred.", "Compare", MB_OK | MB_ICONWARNING);
	}

	return result;
}
