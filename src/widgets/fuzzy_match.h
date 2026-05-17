#pragma once

#include <QString>
#include <QVector>
#include <cstring>

namespace rcx {

// Two-pass strict matcher. Replaces an older loose subsequence scorer
// that, e.g., matched "Test" against "TerminateSomething" (T·e·s·t scattered)
// — that approach yielded hundreds of false positives in a real symbol
// list. The new rules:
//
//   PASS 1 — contiguous substring (case-insensitive). Highest score; the
//            common case ("test" → "TestSet", "Proc" → "GetProcAddress").
//
//   PASS 2 — word-start initials. Pattern characters must each match a
//            "matchable" position in text. Matchable positions are:
//              • index 0
//              • after `_`, `:`, `.`, `-`, space
//              • CamelCase boundary (upper after lower)
//              • letter→digit transition (e.g. "u" then "32" in "uint32_t")
//              • digit→letter transition
//              • every digit (so "u32" → "uint32_t" matches the '3' and '2')
//            All pattern chars must hit matchable positions in order. This
//            rejects the "T·e·s·t" subsequence noise but still accepts
//            "GPA" → "GetProcAddress" and "u32" → "uint32_t".
//
// outPositions receives the matched character indices in `text` so the
// delegate can paint highlight backgrounds at exactly those positions.

inline constexpr int kMaxFuzzyLen = 64;

inline int fuzzyScore(const QString& pattern, const QString& text,
                      QVector<int>* outPositions = nullptr) {
    const int pLen = pattern.size();
    const int tLen = text.size();
    if (pLen == 0) return 1;
    if (pLen > tLen) return 0;
    if (tLen > 4096) return 0;  // sanity cap on absurdly long names

    // ── Pass 1: contiguous substring (case-insensitive) ──
    int idx = text.indexOf(pattern, 0, Qt::CaseInsensitive);
    if (idx >= 0) {
        if (outPositions) {
            outPositions->resize(pLen);
            for (int i = 0; i < pLen; i++) (*outPositions)[i] = idx + i;
        }
        // Score tiers (higher is better):
        //   prefix-of-text (idx==0)    base + 500
        //   after _/./:/- separator     base + 200
        //   at CamelCase boundary       base + 150
        //   word-internal substring     base only
        // Plus tightness bonus (smaller text gap = more focused).
        int score = 1000;
        if (idx == 0) {
            score += 500;
        } else {
            QChar prev = text[idx - 1];
            if (prev == QLatin1Char('_') || prev == QLatin1Char(' ')
                || prev == QLatin1Char(':') || prev == QLatin1Char('.')
                || prev == QLatin1Char('-')) {
                score += 200;
            } else if (text[idx].isUpper() && prev.isLower()) {
                score += 150;
            }
        }
        score += qMax(0, 100 - (tLen - pLen));
        if (pLen == tLen) score += 200;
        return score;
    }

    // ── Pass 2: word-start initials ──
    // Build the "matchable position" set for text and walk pattern through it.
    auto matchable = [&](int i) {
        if (i == 0) return true;
        QChar c = text[i];
        QChar p = text[i - 1];
        if (p == QLatin1Char('_') || p == QLatin1Char(' ')
            || p == QLatin1Char(':') || p == QLatin1Char('.')
            || p == QLatin1Char('-')) return true;
        if (c.isUpper() && p.isLower()) return true;
        if (c.isDigit() && p.isLetter()) return true;
        if (c.isLetter() && p.isDigit()) return true;
        if (c.isDigit()) return true;
        return false;
    };

    QVector<int> hits;
    hits.reserve(pLen);
    int ti = 0;
    for (int pi = 0; pi < pLen; pi++) {
        QChar pc = pattern[pi].toLower();
        bool found = false;
        while (ti < tLen) {
            if (matchable(ti) && text[ti].toLower() == pc) {
                hits.append(ti);
                ti++;
                found = true;
                break;
            }
            ti++;
        }
        if (!found) return 0;
    }

    if (outPositions) *outPositions = hits;
    // Score tiers for initials:
    //   first hit at index 0          → score ~ 600
    //   first hit at later word-start → score ~ 400
    //   plus tightness bonus
    int score = (hits.first() == 0) ? 600 : 400;
    int span = hits.last() - hits.first() + 1;
    score += qMax(0, 50 - (span - pLen));
    return qMax(1, score);
}

} // namespace rcx
