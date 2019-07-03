#pragma once

#define USE_GOOGLE_DENSE_HASH_MAP
#define ENABLE_TEST
#define IO_OPERATIONS

#undef USE_GOOGLE_DENSE_HASH_MAP
#undef ENABLE_TEST
//#undef IO_OPERATIONS

#include <iostream>
#include <string>
#include <vector>
#include <regex>
#include <algorithm>
#include <math.h>
#include <chrono>
#include <fstream>
#include <list>
#include <numeric>

#ifdef IO_OPERATIONS
#include <dlib/serialize.h>
#endif

#ifdef USE_GOOGLE_DENSE_HASH_MAP
#include <sparsehash/dense_hash_map>
#include <sparsehash/dense_hash_set>

#define CUSTOM_SET google::dense_hash_set
#define CUSTOM_MAP google::dense_hash_map
#else
#include <map>
#include <set>

#define CUSTOM_SET std::set
#define CUSTOM_MAP std::map
#endif

#define getHastCode(term) std::hash<std::wstring>()(term)

class dictionaryItem
{
public:
    std::vector<size_t> suggestions;
    size_t count = 0;

    dictionaryItem(size_t c)
    {
        count = c;
    }

    dictionaryItem()
    {
        count = 0;
    }
    ~dictionaryItem()
    {
        suggestions.clear();
    }
};

enum ItemType { NONE, DICT, INTEGER };

class dictionaryItemContainer
{
public:
    dictionaryItemContainer() : itemType(NONE), intValue(0)
    {
    }

    int itemType;
    size_t intValue;
    std::shared_ptr<dictionaryItem> dictValue;
};

namespace dlib {
    inline void
        serialize(const dictionaryItem& item, std::ostream& out)
    {
        try {
            serialize(item.suggestions, out);
            serialize(item.count, out);
        } catch (serialization_error& e) {
            throw serialization_error(fmt::format("{}\n while serializaing an ojbect of type dictionaryItem", e.info));
        }
    }

    inline void
        deserialize(dictionaryItem& item, std::istream& in)
    {
        try {
            deserialize(item.suggestions, in);
            deserialize(item.count, in);
        } catch (serialization_error& e) {
            throw serialization_error(fmt::format("{}\n while deserializing on object of type dictionaryItem", e.info));
        }
    }
    inline void
        serialize(const dictionaryItemContainer& container, std::ostream& out)
    {
        try {
            serialize(container.itemType, out);
            serialize(container.intValue, out);
            if (container.itemType == DICT)
                serialize(*container.dictValue, out);
        } catch (serialization_error& e) {
            throw serialization_error(fmt::format("{}\n while serializaing an ojbect of type dictionaryItemContainer", e.info));
        }
    }

    inline void
        deserialize(dictionaryItemContainer& container, std::istream& in)
    {
        try {
            deserialize(container.itemType, in);
            deserialize(container.intValue, in);
            if (container.itemType == DICT) {
                dictionaryItem item;
                deserialize(item, in);
                container.dictValue = std::make_shared<dictionaryItem>(item);
            }
        } catch (serialization_error& e) {
            throw serialization_error(fmt::format("{}\n while deserializing on object of type ItemType", e.info));
        }
    }
}

class suggestItem
{
public:
    std::wstring term;
    unsigned short distance = 0;
    unsigned short count;

    bool operator== (const suggestItem & item) const
    {
        return term.compare(item.term) == 0;
    }

    size_t HastCode() const
    {
        return std::hash<std::wstring>()(term);
    }
};

class SymSpell
{
public:
    size_t verbose = 0;
    size_t editDistanceMax = 2;

    std::unordered_map<std::wstring, std::wstring> cleanedWords;

    SymSpell()
    {
        setlocale(LC_ALL, "");
#ifdef USE_GOOGLE_DENSE_HASH_MAP
        dictionary.set_empty_key(0);
#endif
    }

    void CreateDictionary(const std::wstring& corpus)
    {
        std::wifstream sr(corpus);

        if (!sr.good()) {
            std::wcout << "File not found: " << corpus;
            return;
        }

        std::wcout << "Creating dictionary ..." << std::endl;

        long wordCount = 0;

        for (std::wstring line; std::getline(sr, line);) {

            for (const auto& key : parseWords(line)) {
                if (CreateDictionaryEntry(key)) ++wordCount;
            }
        }

        sr.close();
    }

#ifdef IO_OPERATIONS

    void Save(std::string filePath)
    {
        dlib::serialize(filePath) << verbose << editDistanceMax << maxlength << dictionary << wordlist;
    }

    void Load(std::string filePath)
    {
        dlib::deserialize(filePath) >> verbose >> editDistanceMax >> maxlength >> dictionary >> wordlist;
    }

#endif

    bool CreateDictionaryEntry(const std::wstring& str)
    {
        const auto key = boost::to_lower_copy(str);
        //std::cout << key << std::endl;
        //std::cout.flush();

        bool result = false;
        dictionaryItemContainer value;

        auto dictionaryEnd = dictionary.end(); // for performance
        auto valueo = dictionary.find(getHastCode(key));
        if (valueo != dictionaryEnd) {
            value = valueo->second;

            if (valueo->second.itemType == ItemType::INTEGER) {
                value.itemType = ItemType::DICT;
                value.dictValue = std::make_shared<dictionaryItem>();
                value.dictValue->suggestions.push_back(valueo->second.intValue);
            } else
                value = valueo->second;

            if (value.dictValue->count < INT_MAX)
                ++(value.dictValue->count);
        } else if (wordlist.size() < INT_MAX) {
            value.itemType = ItemType::DICT;
            value.dictValue = std::make_shared<dictionaryItem>();
            ++(value.dictValue->count);
            const auto mapKey = key;
            dictionary.insert(std::pair<size_t, dictionaryItemContainer>(getHastCode(mapKey), value));
            dictionaryEnd = dictionary.end(); // for performance

            if (key.size() > maxlength)
                maxlength = key.size();
        }

        if (value.dictValue->count == 1) {
            wordlist.push_back(key);
            size_t keyint = wordlist.size() - 1;

            result = true;

            auto deleted = CUSTOM_SET<std::wstring>();
#ifdef USE_GOOGLE_DENSE_HASH_MAP
            deleted.set_empty_key("");
#endif

            Edits(key, deleted);

            for (const auto& del : deleted) {
                auto value2 = dictionary.find(getHastCode(del));
                if (value2 != dictionaryEnd) {
                    if (value2->second.itemType == ItemType::INTEGER) {
                        value2->second.itemType = ItemType::DICT;
                        value2->second.dictValue = std::make_shared<dictionaryItem>();
                        value2->second.dictValue->suggestions.push_back(value2->second.intValue);
                        dictionary[getHastCode(del)].dictValue = value2->second.dictValue;

                        if (std::find(value2->second.dictValue->suggestions.begin(), value2->second.dictValue->suggestions.end(), keyint) == value2->second.dictValue->suggestions.end())
                            AddLowestDistance(value2->second.dictValue, key, keyint, del);
                    } else if (std::find(value2->second.dictValue->suggestions.begin(), value2->second.dictValue->suggestions.end(), keyint) == value2->second.dictValue->suggestions.end())
                        AddLowestDistance(value2->second.dictValue, key, keyint, del);
                } else {
                    dictionaryItemContainer tmp;
                    tmp.itemType = ItemType::INTEGER;
                    tmp.intValue = keyint;

                    dictionary.insert(std::pair<size_t, dictionaryItemContainer>(getHastCode(del), tmp));
                    dictionaryEnd = dictionary.end();
                }
            }
        }

        const auto cleaned = boost::erase_all_copy(key, L" ");
        if (cleaned != key) {
            CreateDictionaryEntry(cleaned);
            cleanedWords.emplace(std::make_pair(cleaned, key));
        }
        return result;
    }

    std::vector<suggestItem> Correct(const std::wstring& str)
    {
        const auto input = boost::to_lower_copy(str);

        std::vector<suggestItem> suggestions;

#ifdef ENABLE_TEST
        using namespace std::chrono;

        high_resolution_clock::time_point t1 = high_resolution_clock::now();

        for (size_t i = 0; i < 100000; ++i) {
            Lookup(input, editDistanceMax);
        }

        high_resolution_clock::time_point t2 = high_resolution_clock::now();
        duration<double> time_span = duration_cast<duration<double>>(t2 - t1);

        std::cout << "It took me " << time_span.count() << " seconds.";
        std::cout << std::endl;
#endif
        suggestions = Lookup(input, editDistanceMax);
        return suggestions;

    }

private:
    size_t maxlength = 0;
    CUSTOM_MAP<size_t, dictionaryItemContainer> dictionary;
    std::vector<std::wstring> wordlist;

    static std::unordered_map<wchar_t, std::set<wchar_t>> sameletters;

    std::vector<std::wstring> parseWords(std::wstring text) const
    {
        std::vector<std::wstring> returnData;

        std::transform(text.begin(), text.end(), text.begin(), ::towlower);
        std::wregex word_regex(L"[^\\W\\d_]+");
        auto words_begin = std::wsregex_iterator(text.begin(), text.end(), word_regex);
        auto words_end = std::wsregex_iterator();

        for (std::wsregex_iterator i = words_begin; i != words_end; ++i) {
            std::wsmatch match = *i;
            returnData.push_back(match.str());
        }

        return returnData;
    }

    void AddLowestDistance(std::shared_ptr<dictionaryItem> const & item, std::wstring suggestion, size_t suggestionint, std::wstring del)
    {
        if ((verbose < 2) && (item->suggestions.size() > 0) && (wordlist[item->suggestions[0]].size() - del.size() > suggestion.size() - del.size()))
            item->suggestions.clear();

        if ((verbose == 2) || (item->suggestions.size() == 0) || (wordlist[item->suggestions[0]].size() - del.size() >= suggestion.size() - del.size()))
            item->suggestions.push_back(suggestionint);
    }

    void Edits(const std::wstring& word, CUSTOM_SET<std::wstring> & deletes) const
    {
        CUSTOM_MAP<size_t, const wchar_t *> queue;
#ifdef USE_GOOGLE_DENSE_HASH_MAP
        queue.set_empty_key(0);
#endif
        queue.insert(std::pair<size_t, const wchar_t*>(getHastCode(word), word.c_str()));

        for (size_t d = 0; d < editDistanceMax; ++d) {
            CUSTOM_MAP<size_t, const wchar_t *> tempQueue;
            auto tempQueueEnd = tempQueue.end();
#ifdef USE_GOOGLE_DENSE_HASH_MAP
            tempQueue.set_empty_key(0);
#endif

            for (auto item : queue) {
                if (wcslen(item.second)) {
                    for (size_t i = 0; i < wcslen(item.second); ++i) {
                        // For Performance ->
                        auto del = static_cast<wchar_t *>(malloc(sizeof(wchar_t*) * (wcslen(item.second) + 1)));

                        wcscpy(del, item.second);
                        size_t k = i;
                        int len = wcslen(item.second);
                        for (; k < len - 1; k++)
                            del[k] = item.second[k + 1];
                        del[k] = L'\0';
                        // <- For Performance

                        if (!deletes.count(del))
                            deletes.insert(del);

                        if (tempQueue.find(getHastCode(del)) == tempQueueEnd) {
                            tempQueue.insert(std::pair<size_t, const wchar_t*>(getHastCode(del), del));
                            tempQueueEnd = tempQueue.end();
                        }
                    }
                }
            }
            queue = tempQueue;
        }
    }

    std::vector<suggestItem> Lookup(const std::wstring& input, size_t editDistanceMax)
    {
        if (input.size() - editDistanceMax > maxlength)
            return std::vector<suggestItem>();

        std::vector<std::wstring> candidates;
        candidates.reserve(2048);
        CUSTOM_SET<size_t> hashset1;
#ifdef USE_GOOGLE_DENSE_HASH_MAP
        hashset1.set_empty_key(0);
#endif

        std::vector<suggestItem> suggestions;
        CUSTOM_SET<size_t> hashset2;
#ifdef USE_GOOGLE_DENSE_HASH_MAP
        hashset2.set_empty_key(0);
#endif

        //object valueo;

        candidates.push_back(input);
        auto dictionaryEnd = dictionary.end();

        size_t candidatesIndexer = 0; // for performance
        while ((candidates.size() - candidatesIndexer) > 0) {
            const auto candidate = candidates[candidatesIndexer];
            size_t candidateSize = candidate.size(); // for performance
            ++candidatesIndexer;

            if ((verbose < 2) && (suggestions.size() > 0) && (input.size() - candidateSize > suggestions[0].distance))
                goto sort;

            auto valueo = dictionary.find(getHastCode(candidate));

            //read candidate entry from dictionary
            if (valueo != dictionaryEnd) {
                if (valueo->second.itemType == ItemType::INTEGER) {
                    valueo->second.itemType = ItemType::DICT;
                    valueo->second.dictValue = std::make_shared<dictionaryItem>();
                    valueo->second.dictValue->suggestions.push_back(valueo->second.intValue);
                }


                if (valueo->second.itemType == ItemType::DICT &&
                    valueo->second.dictValue->count > 0 &&
                    hashset2.insert(getHastCode(candidate)).second) {
                    //add correct dictionary term term to suggestion list
                    suggestItem si;
                    si.term = candidate;
                    si.count = valueo->second.dictValue->count;
                    si.distance = input.size() - candidateSize;
                    suggestions.push_back(si);
                    //early termination
                    if ((verbose < 2) && (input.size() - candidateSize == 0))
                        goto sort;
                }

                for (size_t suggestionint : valueo->second.dictValue->suggestions) {
                    //save some time
                    //skipping double items early: different deletes of the input term can lead to the same suggestion
                    //index2word
                    const auto suggestion = wordlist[suggestionint];
                    if (hashset2.insert(getHastCode(suggestion)).second) {
                        size_t distance = 0;
                        if (suggestion != input) {
                            if (suggestion.size() == candidateSize) distance = input.size() - candidateSize;
                            else if (input.size() == candidateSize) distance = suggestion.size() - candidateSize;
                            else {
                                size_t ii = 0;
                                size_t jj = 0;
                                while ((ii < suggestion.size()) && (ii < input.size()) && (suggestion[ii] == input[ii]))
                                    ++ii;

                                while ((jj < suggestion.size() - ii) && (jj < input.size() - ii) && (suggestion[suggestion.size() - jj - 1] == input[input.size() - jj - 1]))
                                    ++jj;
                                if ((ii > 0) || (jj > 0))
                                    distance = DamerauLevenshteinDistance(suggestion.substr(ii, suggestion.size() - ii - jj), input.substr(ii, input.size() - ii - jj));
                                else
                                    distance = DamerauLevenshteinDistance(suggestion, input);

                            }
                        }

                        if ((verbose < 2) && (suggestions.size() > 0) && (suggestions[0].distance > distance))
                            suggestions.clear();

                        if ((verbose < 2) && (suggestions.size() > 0) && (distance > suggestions[0].distance))
                            continue;

                        if (distance <= editDistanceMax) {
                            auto value2 = dictionary.find(getHastCode(suggestion));

                            if (value2 != dictionaryEnd) {
                                suggestItem si;
                                si.term = suggestion;
                                if (value2->second.itemType == ItemType::DICT)
                                    si.count = value2->second.dictValue->count;
                                else
                                    si.count = 1;
                                si.distance = distance;
                                suggestions.push_back(si);
                            }
                        }
                    }
                }
            }


            if (input.size() - candidateSize < editDistanceMax) {
                if ((verbose < 2) && (suggestions.size() > 0) && (input.size() - candidateSize >= suggestions[0].distance))
                    continue;

                for (size_t i = 0; i < candidateSize; ++i) {
                    auto wordClone = candidate;
                    const auto& del = wordClone.erase(i, 1);
                    if (hashset1.insert(getHastCode(del)).second)
                        candidates.push_back(del);
                }
            }
        }//end while

        //sort by ascending edit distance, then by descending word frequency
    sort:
        if (verbose < 2)
            sort(suggestions.begin(), suggestions.end(), Xgreater1());
        else
            sort(suggestions.begin(), suggestions.end(), Xgreater2());

        for (auto& suggestion : suggestions) {
            if (cleanedWords.find(suggestion.term) != cleanedWords.end())
                suggestion.term = cleanedWords.at(suggestion.term);
        }

        if ((verbose == 0) && (suggestions.size() > 1))
            return std::vector<suggestItem>(suggestions.begin(), suggestions.begin() + 1);
        else
            return suggestions;
    }

    struct Xgreater1
    {
        bool operator()(const suggestItem& lx, const suggestItem& rx) const
        {
            return lx.count > rx.count;
        }
    };

    struct Xgreater2
    {
        bool operator()(const suggestItem& lx, const suggestItem& rx) const
        {
            auto cmpForLx = 2 * (lx.distance - rx.distance) - (lx.count - rx.count);
            auto cmpForRx = 2 * (rx.distance - lx.distance) - (rx.count - lx.count);
            return cmpForLx > cmpForRx;
        }
    };
public:

    static std::wstring
    DisassembleHangul(const wchar_t letter)
    {
        if (letter < L'°¡' || letter > L'ÆR')
            return std::wstring(1, letter);

        const auto chosung = static_cast<wchar_t>(0x1100 + ((letter - 0xAC00) / (28 * 21)));
        const auto jungsung = static_cast<wchar_t>(0x1161 + (((letter - 0xAC00) % (28 * 21)) / 28));
        const auto jongsung = static_cast<wchar_t>(0x11A8 + (((letter - 0xAC00) % 28) - 1));

        auto decomposed = std::wstring(1, chosung) + jungsung;
        
        if (jongsung != 0x11A7) {
            decomposed += jongsung;
        }

        return decomposed;
    }

    static std::wstring
    DisassembleHangul(const std::wstring& string)
    {
        const auto decomposed = std::accumulate(std::begin(string), std::end(string), std::wstring(), [&](const std::wstring& str, const wchar_t ch) {
            return str + DisassembleHangul(ch);
        });

        return decomposed;
    }

    static std::wstring
        ToWideString(const std::string& str, int codepage = CP_ACP)
    {
        const auto num_chars = MultiByteToWideChar(codepage, 0, str.data(), str.size(), nullptr, 0);
        std::wstring buffer(num_chars, L'\0');
        MultiByteToWideChar(codepage, 0, str.data(), str.size(), const_cast<wchar_t*>(buffer.data()), num_chars);
        return buffer;
    }

    
    static size_t DamerauLevenshteinDistanceHangul(const std::wstring &ss1, const std::wstring &ss2)
    {
        if (sameletters.empty()) {
            static const std::vector<std::vector<wchar_t>> SAMES {
                std::vector<wchar_t>{L'0', L'o', L'\x110B', L'\x11BC',  L'\x1106', L'\x11B7'},
                std::vector<wchar_t>{L'1', L'l', L'i', L'\x1175', L'\x1161', L'\x1165'}
            };

            for (const auto& letters : SAMES) {
                for (auto i = 0; i < letters.size(); i++) {
                    std::set<wchar_t> duplicated;
                    for (auto j = 0; j < letters.size(); j++) {
                        if (i == j)
                            continue;
                        duplicated.emplace(letters[j]);
                    }

                    if (sameletters.find(letters[i]) == sameletters.end()) {
                        sameletters.emplace(std::make_pair(letters[i], duplicated));
                    } else {
                        sameletters[letters[i]].insert(std::begin(duplicated), std::end(duplicated));
                    }
                }
            }
        }

        const auto s1 = DisassembleHangul(ss1);
        const auto s2 = DisassembleHangul(ss2);

        const size_t m(s1.size());
        const size_t n(s2.size());

        if (m == 0) return n;
        if (n == 0) return m;

        size_t *costs = new size_t[n + 1];

        for (size_t k = 0; k <= n; ++k) costs[k] = k;

        size_t i = 0;
        auto s1End = s1.end();
        auto s2End = s2.end();
        for (std::wstring::const_iterator it1 = s1.begin(); it1 != s1End; ++it1, ++i) {
            costs[0] = i + 1;
            size_t corner = i;

            size_t j = 0;
            for (std::wstring::const_iterator it2 = s2.begin(); it2 != s2End; ++it2, ++j) {
                size_t upper = costs[j + 1];
                if (*it1 == *it2 || (sameletters.find(*it1) != sameletters.end() && sameletters.at(*it1).find(*it2) != sameletters.at(*it1).end())) {
                    costs[j + 1] = corner;
                } else {
                    size_t t(upper < corner ? upper : corner);
                    costs[j + 1] = (costs[j] < t ? costs[j] : t) + 1;
                }

                corner = upper;
            }
        }

        size_t result = costs[n];
        delete[] costs;

        return result;
    }

    static size_t DamerauLevenshteinDistance(const std::wstring &s1, const std::wstring &s2)
    {
        if (s1 == s2) return 0;

        const size_t m(s1.size());
        const size_t n(s2.size());

        if (m == 0) return n;
        if (n == 0) return m;

        size_t *costs = new size_t[n + 1];

        for (size_t k = 0; k <= n; ++k) costs[k] = k;

        size_t i = 0;
        auto s1End = s1.end();
        auto s2End = s2.end();
        for (auto it1 = s1.begin(); it1 != s1End; ++it1, ++i) {
            costs[0] = i + 1;
            size_t corner = i;

            size_t j = 0;
            for (auto it2 = s2.begin(); it2 != s2End; ++it2, ++j) {
                size_t upper = costs[j + 1];
                if (*it1 == *it2) {
                    costs[j + 1] = corner;
                } else {
                    size_t t(upper < corner ? upper : corner);
                    costs[j + 1] = (costs[j] < t ? costs[j] : t) + 1;
                }

                corner = upper;
            }
        }

        size_t result = costs[n];
        delete[] costs;

        if (result != 0)
            result = std::min(result, DamerauLevenshteinDistanceHangul(s1, s2));

        return result;
    }

};

std::unordered_map<wchar_t, std::set<wchar_t>> SymSpell::sameletters;