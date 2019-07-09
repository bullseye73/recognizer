#ifndef __SERVICE_MODULE__
#define __USE_CACHE__ true
#define __USE_INPROC__ true
#define __USE_ENGINE__ 0
#else
#define __USE_CACHE__ false
#define __USE_INPROC__ false
#endif
#define __CACHE_DIRCTORY__ L"C:\\workspace\\vs2015\\OUTPUT\\cache"
#define __LOG_FILE_NAME__ L"C:\\workspace\\vs2015\\OUTPUT\\log_result.txt"

#define NOMINMAX
#define LOG_USE_WOFSTREAM
#define WRITE_TRADE_TXT
#define WRITE_FILE_DIR L"C:\\workspace\\vs2015\\OUTPUT\\result"
#define DEFAULT_FAX_COLUMN_VALUE_FOR_CODE L""
#define DEFAULT_FAX_COLUMN_VALUE_FOR_STRING L"N/A"
#define DEFAULT_FAX_COLUMN_VALUE_FOR_PRD L"999"
#define DEFAULT_FAX_COLUMN_VALUE_FOR_OTHER L""


#include <windows.h>
#include <atlsafe.h>

#include <memory>
#include <chrono>
#include <thread>
#include <strstream>
#include <numeric>
#include <unordered_set>
#include <mutex>

#include <boost/regex.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/range/algorithm/replace_if.hpp>
#include <boost/range/algorithm/remove_if.hpp>
#include <boost/range/algorithm_ext/erase.hpp>
#include <boost/locale.hpp>


#include <yaml-cpp/yaml.h>
#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
/*
#include <tiff.h>
#include <tiffio.h>
#include <tiffiop.h>
#include <tiffio.hxx>
*/


#include <text/csv/istream.hpp>

#include "aho_corasick.hpp"
#include "symspell.h"
#include "naturalorder.h"

#include "types.hpp"
#include "utils.hpp"
#include "recognizer.hpp"
#include "encryption.h"

#import "FREngine.tlb"
#include <locale>
#include <codecvt>

namespace selvy {
	namespace ocr {
		int to_confidence(const std::vector<std::vector<int>>& confidences)
		{
			int sum = 0;
			int count = 0;
			for (const auto& confidence : confidences) {
				for (const auto& c : confidence) {
					sum += c;
					count++;
				}
			}

			return static_cast<float>(sum / std::max(1, count));
		}
#if defined(_DEBUG)
		static cv::Mat debug_;
#endif

		class memory_reader : public FREngine::IReadStream {
		public:
			ULONG ref_count_ = 0;
			std::stringstream stream_;
			std::string decrypted_;

			memory_reader(const std::string& buffer, const std::string& key)
			{
				//decrypted_ = decrypt_image(buffer, key);

				//if (decrypted_[0] == 'I' && decrypted_[1] == 'I') {
				//	/*std::istringstream istr(decrypted_);
				//	auto tiff_handle = TIFFStreamOpen("MemTIFF", &istr);
				//	istr.clear();
				//	if (tiff_handle) {
				//		tdata_t buf;
				//		tstrip_t strip;
				//		uint32* bc;
				//		uint32 stripsize;

				//		TIFFGetField(tiff_handle, TIFFTAG_STRIPBYTECOUNTS, &bc);
				//		stripsize = bc[0];

				//		buf = TIFFmalloc(stripsize);
				//		for (strip = 0; strip < TIFFNumberOFSrips(tiff_handle); strip++) {
				//			if (bc[strip] > stripsize) {
				//				buf = _TIFFrealloc(buf, bc[strip)]);
				//				stripsize = bc[strip];
				//			}
				//			TIFFReadRawStrip(tiff_handle, strip, buf, bc[strip]);
				//		}

				//		stream_.write((char *)buf, stripsize);

				//		decrypted_ = std::string((char*)buf, (char*)buf + stripsize);

				//		_TIFFfree(buf);
				//		TIFFClose(tiff_handle);
				//	}*/
				//}
				//else {
				decrypted_ = buffer;
				stream_ = std::stringstream(buffer);
				//}

#if defined(_DEBUG)
				std::vector<char> image_buffer{ std::begin(buffer), std::end(buffer) };
				debug_ = cv::imdecode(cv::Mat(image_buffer), cv::IMREAD_COLOR);
#endif
			}

			memory_reader(const boost::filesystem::path& file, const std::string& key) : memory_reader(
				[&]() {
				std::ifstream ifs(file.native(), std::ios::binary | std::ios::in);
				const std::string buffer((std::istreambuf_iterator<char>(ifs)), (std::istreambuf_iterator<char>()));
				ifs.close(); return buffer; }(), key)
			{
			}


				virtual ~memory_reader() = default;

				HRESULT
					QueryInterface(const IID& riid, void** ppvobject) override
				{
					*ppvobject = nullptr;

					const auto uuid = __uuidof(FREngine::IReadStream);

					if (riid == IID_IUnknown || riid == __uuidof(FREngine::IReadStream)) {
						*ppvobject = this;
						AddRef();
						return S_OK;
					}

					return E_NOINTERFACE;
				}

				ULONG
					AddRef() override
				{
					return InterlockedIncrement(&ref_count_);
				}

				ULONG
					Release() override
				{
					if (InterlockedDecrement(&ref_count_) == 0) {
						return 0;
					}

					return ref_count_;
				}

				HRESULT
					raw_Read(SAFEARRAY** data, int count, int* bytesRead) override
				{
					CComSafeArray<unsigned char> buffer(count);
					stream_.read(static_cast<char*>(buffer.m_psa->pvData), count);
					*bytesRead = stream_.gcount();
					*data = buffer.Detach();
					return S_OK;
				}

				HRESULT
					raw_Close() override
				{
					return S_OK;
				}
		};

		static std::unordered_map<std::wstring, std::unordered_map<std::wstring, std::tuple<std::map<std::wstring, double>, std::map<std::wstring, double>, int, int>>> weights_;
		static std::unordered_map<std::wstring, std::unordered_map<std::wstring, std::vector<std::wstring>>> dictionaries_;
		static std::unordered_map < std::wstring, std::unordered_map <
			std::wstring, std::unordered_map<std::wstring, std::vector<std::wstring>> >> keywords_;
		static std::string secret_key_;

		static std::mutex locks_;
		static std::pair<std::vector<std::pair<CComPtr<FREngine::IEngineLoader>, FREngine::IEnginePtr>>, std::vector<std::pair<CComPtr<FREngine::IEngineLoader>, FREngine::IEnginePtr>>> engine_pool_;

		void
			allocate_engine_pool(const configuration& configuration, bool inproc)
		{
#ifdef __USE_ENGINE_POOL__
			locks_.lock();

			CLSID cls_id;
			if (inproc)
				CLSIDFromProgID(L"FREngine.InprocLoader", &cls_id);
			else
				CLSIDFromProgID(L"FREngine.OutprocLoader", &cls_id);

			std::vector<std::pair<CComPtr<FREngine::IEngineLoader>, FREngine::IEnginePtr>> free_engine_pool;

#ifndef __SERVICE_MODULE__
			const auto engine_counts = 1;
#else
			const auto engine_counts = 1;
#endif
			for (auto i = 0; i < engine_counts; i++) {
				CComPtr<FREngine::IEngineLoader> loader;
				FREngine::IEnginePtr engine;

				do {
					try {
						if (loader == nullptr)
							loader.CoCreateInstance(cls_id, nullptr, inproc ? CLSCTX_INPROC_SERVER : CLSCTX_LOCAL_SERVER);

						engine = loader->InitializeEngine(configuration.at(L"engine").at(L"license").c_str(), L"", L"", L"", L"", VARIANT_FALSE);
					}
					catch (_com_error& e) {
						if (loader)
							loader->ExplicitlyUnload();
						std::this_thread::sleep_for(std::chrono::seconds(1));
					}
				} while (engine == nullptr);

				free_engine_pool.emplace_back(std::make_pair(loader, engine));
			}

			engine_pool_ = std::make_pair(free_engine_pool, std::vector<std::pair<CComPtr<FREngine::IEngineLoader>, FREngine::IEnginePtr>>());

			locks_.unlcok();
#endif
		}

		void deallocate_engine_pool()
		{
#ifdef __USE_ENGINE_POOL__
			locks_.lock();

			std::vector<std::pair<CComPtr<FREngine::IEngineLoader>, FREngine::IEnginePtr>> free_engine_pool;
			std::vector<std::pair<CComPtr<FREngine::IEngineLoader>, FREngine::IEnginePtr>> used_engine_pool;
			std::tie(free_engine_pool, used_engine_pool) = engine_pool_;

			for (auto& engine_handle : free_engine_pool) {
				CComPtr<FREngine::IEngineLoader> loader;
				FREngine::IEnginePtr engine;
				std::tie(loader, engine) = engine_handle;

				if (loader)
					loader->ExplicitlyUnload();
			}

			engine_pool_.first.clear();

			for (auto& engine_handle : used_engine_pool) {
				CComPtr<FREngine::IEngineLoader> loader;
				FREngine::IEnginePtr engine;
				std::tie(loader, engine) = engine_handle;

				if (loader)
					loader->ExplicitlyUnload();
			}

			engine_pool_.second.clear();

			locks_.unlock();
#endif
		}

#ifdef __USE_ENGINE_POOL__
		std::pair<CComPtr<FREngine::IEngineLoader>, FREngine::IEnginePtr>
			get_engine_object(const configuration& configuation)
		{
			locks_.lock();

			CComPtr<FREngine::IEngineLoader> loader;
			FREngine::IEnginePtr engine;

			do {
				std::vector<std::pair<CComPtr<FREngine::IEngineLoader>, FREngine::IEnginePtr>> free_engine_pool;
				std::vector<std::pair<CComPtr<FREngine::IEngineLoader>, FREngine::IEnginePtr>> used_engine_pool;
				std::tie(free_engine_pool, used_engine_pool) = engine_pool_;

				if (!free_engine_pool.empty()) {
					std::tie(loader, engine) = free_engine_pool.back();
					free_engine_pool.pop_back();
					used_engine_pool.push_back(std::make_pair(loader, engine));
					break;
				}

				std::this_thread::sleep_for(std::chrono::seconds(1));
			} while (engine == nullptr);

			locks_.unlock();
			return std::make_pair(loader, engine);
		}
#else
		std::pair<CComPtr<FREngine::IEngineLoader>, FREngine::IEnginePtr>
			get_engine_object(const configuration& configuration, bool inproc = __USE_INPROC__)
		{
			CLSID cls_id;
			if (inproc)
				CLSIDFromProgID(L"FREngine.InprocLoader", &cls_id);
			else
				CLSIDFromProgID(L"FREngine.OutprocLoader", &cls_id);

			CComPtr<FREngine::IEngineLoader> loader;
			FREngine::IEnginePtr engine;

			do {
				try {
					if (loader == nullptr)
						loader.CoCreateInstance(cls_id, nullptr, inproc ? CLSCTX_INPROC_SERVER : CLSCTX_LOCAL_SERVER);

					engine = loader->InitializeEngine(configuration.at(L"engine").at(L"license").c_str(), L"", L"", L"", L"", VARIANT_FALSE);
				}
				catch (_com_error& e) {
					if (loader)
						loader->ExplicitlyUnload();
					spdlog::get("recognizer")->info("recognize engine load fail!");
					std::this_thread::sleep_for(std::chrono::seconds(1));
				}
			} while (engine == nullptr && !__USE_CACHE__);
			return std::make_pair(loader, engine);
		}
#endif

		void release_engine_object(const std::pair<CComPtr<FREngine::IEngineLoader>, FREngine::IEnginePtr>& engine_handle)
		{
#ifdef __USE_ENGINE_POOL__
			locks_.lock();
			std::vector<std::pair<CComPtr<FREngine::IEngineLoader>, FREngine::IEnginePtr>> free_engine_pool;
			std::vector<std::pair<CComPtr<FREngine::IEngineLoader>, FREngine::IEnginePtr>> used_engine_pool;
			std::tie(free_engine_pool, used_engine_pool) = engine_pool_;
			boost::remove_erase(used_engine_pool, engine_handle);
			free_engine_pool.emplace_back(engine_handle);
			locks_.unlock();
#else
			CComPtr<FREngine::IEngineLoader> loader;
			FREngine::IEnginePtr engine;
			std::tie(loader, engine) = engine_handle;

			if (loader)
				loader->ExplicitlyUnload();
#endif
		}

		inline std::shared_ptr<SymSpell>
			build_spell_dictionary(const std::vector < std::wstring>& words, const int distance = 2)
		{
			auto dictionary = std::make_shared<SymSpell>();
			dictionary->editDistanceMax = distance;

			std::for_each(std::begin(words), std::end(words), [&dictionary](const std::wstring& word) {
				dictionary->CreateDictionaryEntry(word);
			});

			return dictionary;
		}

		std::unordered_map<std::wstring, std::unordered_map<std::wstring, std::tuple<std::map<std::wstring, double>, std::map<std::wstring, double>, int, int>>>
			load_weights(const std::wstring& data_directory)
		{
			std::unordered_map<std::wstring, std::unordered_map<std::wstring, std::tuple<std::map<std::wstring, double>, std::map<std::wstring, double>, int, int>>> weights;

			const auto load_weight = [&](const boost::filesystem::path& directory, const std::wstring& prefix) {
				const auto weight_file = fmt::format(L"{}\\{}.wgt", directory.native(), prefix);
				const auto text_file = fmt::format(L"{}\\{}.txt", directory.native(), prefix);

				std::map<std::wstring, double> weight_map;
				int weight_totals = 0;
				if (!boost::filesystem::exists(weight_file) || boost::filesystem::last_write_time(weight_file) < boost::filesystem::last_write_time(text_file)) {
					std::wifstream file(text_file);

					if (!file.is_open())
						return std::pair<std::map<std::wstring, double>, int>();
					std::wstring line;
					std::wstring token;
					std::wstring weight;
					while (std::getline(file, line)) {
						std::wstringstream ss(line);
						ss >> token;
						ss >> weight;
						weight_totals += std::stoi(weight);
						weight_map.emplace(std::make_pair(token, std::stod(weight)));
					}
					file.close();
					dlib::serialize(to_utf8(weight_file)) << weight_map << weight_totals;
				}
				else {
					dlib::deserialize(to_utf8(weight_file)) >> weight_map >> weight_totals;
				}
				return std::make_pair(weight_map, weight_totals);
			};

			std::unordered_map<std::wstring, std::tuple<std::map<std::wstring, double>, std::map<std::wstring, double>, int, int>> profile_weights;
			for (auto& entry : boost::filesystem::directory_iterator(data_directory)) {
				if (!boost::filesystem::is_directory(entry))
					continue;

				const auto& profile_directory = entry.path();
				const auto weight_directory = fmt::format(L"{}\\weights", profile_directory.native());

				if (!boost::filesystem::exists(weight_directory))
					continue;

				for (auto& directory_entry : boost::filesystem::directory_iterator(weight_directory)) {
					if (!boost::filesystem::is_directory(directory_entry))
						continue;

					const auto class1 = load_weight(directory_entry, L"positive");
					const auto class2 = load_weight(directory_entry, L"negative");

					profile_weights.emplace(std::make_pair(directory_entry.path().filename().native(), std::make_tuple(std::get<0>(class1), std::get<0>(class2), std::get<1>(class1), std::get<1>(class2))));
				}

				weights.emplace(profile_directory.filename().native(), profile_weights);
			}

			return weights;
		}

		inline std::tuple<std::map<std::wstring, double>, std::map<std::wstring, double>, int, int>
			get_weights(const configuration& configuration,
			const std::unordered_map<std::wstring, std::unordered_map<std::wstring, std::tuple<std::map<std::wstring, double>, std::map<std::wstring, double>, int, int>>>& weight,
			const std::wstring& weight_category)
		{
			const auto profile = configuration.at(L"setting").at(L"profile");
			if (weight.find(profile) == weight.end() || weight.at(profile).find(weight_category) == weight.at(profile).end())
				return std::tuple<std::map<std::wstring, double>, std::map<std::wstring, double>, int, int>();

			return weight.at(profile).at(weight_category);
		}

		bool classify_text(const std::wstring& text, std::tuple<std::map<std::wstring, double>, std::map<std::wstring, double>, int, int>& weights, const double threshold)
		{
			auto lower_text = boost::to_lower_copy(text);
			lower_text = boost::regex_replace(lower_text, boost::wregex(L"[^/ 0-9a-z\\.-]"), L"");
			lower_text = boost::replace_if(lower_text, boost::is_any_of(L"0123456789"), L'*');
			lower_text = boost::trim_copy(lower_text);

			const auto c1 = std::get<0>(weights);
			const auto c2 = std::get<1>(weights);
			const auto c1_word_size = std::get<2>(weights);
			const auto c2_word_size = std::get<3>(weights);

			std::wstringstream ss(lower_text);
			std::wstring token;
			long double c1_p = 0, c2_p = 0;

			while (ss >> token) {
				if (c1.find(token) == c1.end())
					c1_p -= log2((c1_word_size + c1.size() + c2.size()));
				else
					c1_p += log2(c1.at(token) / c1_word_size);

				if (c2.find(token) == c2.end())
					c2_p -= log2((c2_word_size + c1.size() + c2.size()));
				else
					c2_p += log2(c2.at(token) / c2_word_size);
			}

			return (c1_p + log2(threshold) > c2_p + log2(1 - threshold));
		}

		std::unordered_map<std::wstring, std::unordered_map<std::wstring, std::vector<std::wstring>>>
			load_dictionaries(const std::wstring& data_directory)
		{
			std::unordered_map<std::wstring, std::unordered_map<std::wstring, std::vector<std::wstring>>> dictionaries;

			for (auto& entry : boost::filesystem::directory_iterator(data_directory)) {
				if (!boost::filesystem::is_directory(entry))
					continue;

				const auto& profile_directory = entry.path();
				const auto dictionary_file = fmt::format(L"{}\\words.yaml", profile_directory.native());

				if (!boost::filesystem::exists(dictionary_file))
					continue;

				std::unordered_map<std::wstring, std::vector<std::wstring>> profile_dictionaries;
				auto dictionary = YAML::LoadFile(to_cp949(boost::filesystem::absolute(dictionary_file).native()));
				for (auto category = std::begin(dictionary); category != std::end(dictionary); ++category) {
					std::vector<std::wstring> words;
					std::transform(std::begin(category->second), std::end(category->second), std::back_inserter(words),
						[](const YAML::Node& word) {
						return word.as<std::wstring>();
					});

					const auto category_name = category->first.as<std::wstring>();
					const auto spell_dictionary_file = fmt::format(L"{}\\{}.dic", profile_directory.native(), category_name);
					const static std::set<std::wstring> min_distance_category{ L"insurance_companies", };

					if (!boost::filesystem::exists(spell_dictionary_file) || boost::filesystem::last_write_time(spell_dictionary_file) < boost::filesystem::last_write_time(dictionary_file)) {
						auto distance = 2;
						if (min_distance_category.find(category_name) != min_distance_category.end())
							distance = 1;

						auto dictionary = build_spell_dictionary(words, 1);
						dictionary->Save(to_cp949(spell_dictionary_file));
					}

					profile_dictionaries.emplace(std::make_pair(category_name, words));
				}

				dictionaries.emplace(std::make_pair(profile_directory.filename().native(), profile_dictionaries));
			}

			return dictionaries;
		}

		inline std::vector<std::wstring>
			get_dictionary_words(const configuration& configuration,
			const std::unordered_map<std::wstring, std::unordered_map<std::wstring, std::vector<std::wstring>>>&
			dictionary,
			const std::wstring& dictionary_category)
		{
			const auto profile = configuration.at(L"setting").at(L"profile");
			if (dictionary.find(profile) == dictionary.end() || dictionary.at(profile).find(dictionary_category) == dictionary
				.at(profile).end())
				return std::vector<std::wstring>();

			return dictionary.at(profile).at(dictionary_category);
		}

		std::unordered_map < std::wstring, std::unordered_map <
			std::wstring, std::unordered_map<std::wstring, std::vector<std::wstring>> >>
			load_keywords(const std::wstring& data_directory)
		{
			std::unordered_map<std::wstring, std::unordered_map<std::wstring, std::unordered_map<std::wstring, std::vector<std::wstring>>>>
				keywords;
			for (auto& entry : boost::filesystem::directory_iterator(data_directory)) {
				if (!boost::filesystem::is_directory(entry))
					continue;
				std::unordered_map<std::wstring, std::unordered_map<std::wstring, std::vector<std::wstring>>> profile_keywords;

				for (auto& category_entry : boost::filesystem::directory_iterator(fmt::format(L"{}\\categories", entry.path().native()))) {
					if (!boost::filesystem::is_directory(category_entry))
						continue;
					const auto& category_directory = category_entry.path();
					const auto rule_file = fmt::format(L"{}\\rules.yaml", category_directory.native());

					if (!boost::filesystem::exists(rule_file))
						continue;

					std::unordered_map<std::wstring, std::vector<std::wstring>> category_keywords;
					auto rules = YAML::LoadFile(to_cp949(boost::filesystem::absolute(rule_file).native()));
					for (const auto& rule : rules) {
						std::vector<std::wstring> words;
						std::transform(std::begin(rule["keywords"]), std::end(rule["keywords"]), std::back_inserter(words),
							[](const YAML::Node& node) {
							return node.as<std::wstring>();
						});

						category_keywords.emplace(std::make_pair(rule["field"].as<std::wstring>(), words));
					}

					profile_keywords.emplace(std::make_pair(category_entry.path().filename().native(), category_keywords));
				}
				keywords.emplace(std::make_pair(entry.path().filename().native(), profile_keywords));
			}
			return keywords;
		}

		inline std::unordered_map<std::wstring, std::vector<std::wstring>>
			get_keywords(const configuration& configuration,
			const std::unordered_map < std::wstring, std::unordered_map <
			std::wstring, std::unordered_map<std::wstring, std::vector<std::wstring>> >> &keywords,
			const std::wstring& category)
		{
			const auto profile = configuration.at(L"setting").at(L"profile");

			if (keywords.find(profile) == keywords.end() || keywords.at(profile).find(category) == keywords.at(profile).end())
				return std::unordered_map<std::wstring, std::vector<std::wstring>>();

			return keywords.at(profile).at(category);
		}

		inline std::shared_ptr<SymSpell>
			build_spell_dictionary(const configuration& configuration, const std::wstring& category, const int distance = 2)
		{
			const auto dictionary_file = fmt::format(L"{}\\{}.dic", get_profile_directory(configuration), category);
			const auto word_file = fmt::format(L"{}\\words.yaml", get_profile_directory(configuration));

			if (!boost::filesystem::exists(dictionary_file) || boost::filesystem::last_write_time(dictionary_file) < boost::filesystem::last_write_time(word_file)) {
				const auto words = get_dictionary_words(configuration, dictionaries_, category);
				auto dictionary = build_spell_dictionary(words, distance);
				dictionary->Save(to_cp949(dictionary_file));

				return dictionary;
			}

			auto dictionary = std::make_shared<SymSpell>();
			dictionary->Load(to_cp949(dictionary_file));

			return dictionary;
		}

		inline void
			build_trie(aho_corasick::wtrie& trie, const std::vector<std::wstring>& words)
		{
			trie.case_insensitive().remove_overlaps().allow_space();

			std::for_each(std::begin(words), std::end(words), [&trie](const std::wstring& word) {
				trie.insert(word);
			});
		}

		static bool is_document_filtered(const std::string& decrypted)
		{
			std::vector<char> image_buffer{ std::begin(decrypted), std::end(decrypted) };
			auto src = cv::imdecode(cv::Mat(image_buffer), cv::IMREAD_GRAYSCALE);

			if (src.empty() || src.cols < 500)
				return true;

			cv::Mat resized;
			cv::Size resized_size{ 256, 256 };
			if (src.cols > src.rows)
				resized_size.height = (static_cast<double>(resized_size.width) / src.cols) * src.rows;
			else
				resized_size.width = (static_cast<double>(resized_size.height) / src.rows) * src.cols;
			cv::resize(src, resized, resized_size, 0, 0, cv::INTER_AREA);
			cv::Mat binarized;
			cv::threshold(resized, binarized, 0, 255, cv::THRESH_OTSU | cv::THRESH_BINARY_INV);
			const auto count_of_text = static_cast<double>(cv::countNonZero(binarized));
			const auto text_background_ratio = count_of_text / binarized.total();

			if (text_background_ratio > 0.4 || text_background_ratio < 0.015)
				return true;

			return false;
		}

		static std::wstring
			classify_document(const FREngine::IEnginePtr& engine, const configuration& configuration,
			const FREngine::IClassificationEnginePtr& classification_engine,
			FREngine::IModelPtr& model,
			const boost::filesystem::path& file, const FREngine::IFRDocumentPtr& document,
			bool use_cache = __USE_CACHE__)
		{
			cv::TickMeter classification_ticks;
			classification_ticks.start();

			auto need_serialization = false;
			const auto file_name = file.filename().native();
			auto file_name_without_ext = file_name.substr(0, file_name.size() - 4);
			file_name_without_ext = boost::regex_replace(file_name_without_ext, boost::wregex(L"_001"), L"");
			const auto cache_file = boost::filesystem::path(fmt::format(L"{}\\{}.classification.cache", __CACHE_DIRCTORY__, file_name_without_ext)).native();

			std::wstring category;
			double confidence = 0.;

			if (use_cache) {
				if (boost::filesystem::exists(cache_file)) {
					try {
						dlib::deserialize(to_cp949(cache_file)) >> category >> confidence;
						classification_ticks.stop();
						spdlog::get("recognizer")->info("classify document : {} ({:2f}mSec, category : {} ({:.2f}), cache : true)",
							to_cp949(file.filename().native()), classification_ticks.getTimeMilli(),
							to_cp949(category), confidence);
#ifdef LOG_USE_WOFSTREAM
						std::wofstream txt_file;
						txt_file.imbue(std::locale(std::locale::empty(), new std::codecvt_utf8<wchar_t, 0x10ffff, std::generate_header>));
						txt_file.open(__LOG_FILE_NAME__, std::wofstream::out | std::wofstream::app);
						txt_file << L"- category : " << category << std::endl;
						txt_file << L"- confidence : " << confidence << std::endl;
						txt_file.close();
#endif
						if (confidence < 0.65)
							category = L"";

						return category;
					}
					catch (dlib::serialization_error&) {
					}
				}

				need_serialization = true;
			}

#if __USE_ENGINE__
			if (need_serialization) {
				return L"";
			}
#endif

			engine->CleanRecognizerSession();
			engine->LoadPredefinedProfile(L"TextExtraction_Speed");

			auto profile_path = boost::filesystem::path("preset.ini");
			if (!boost::filesystem::exists(profile_path))
				profile_path = boost::filesystem::path(fmt::format(L"{}\\bin\\preset.ini", get_install_path()));

			engine->LoadProfile(boost::filesystem::absolute(profile_path).native().c_str());

			if (document->Pages->Item(0)->Layout != nullptr)
				document->Pages->Item(0)->Layout->Clean();

			document->Analyze(nullptr, nullptr, nullptr);

			document->Recognize(nullptr, nullptr);
			auto classification_object = classification_engine->CreateObjectFromDocument(document);
			auto suitable_classifier = classification_object->SuitableClassifiers;

			if (suitable_classifier & FREngine::ClassifierTypeEnum::CT_Combined == 0) {
				classification_ticks.stop();
				spdlog::get("recognizer")->info("classify document : {} ({:.2f}mSec, category : {} ({:.2f}), cache : false)",
					to_cp949(file.filename().native()),
					classification_ticks.getTimeMilli(), to_cp949(category), confidence);
				return L"";
			}

			const auto classified = model->Classify(classification_object);

			if (classified != nullptr && classified->Count != 0) {
				category = std::wstring(classified->Item(0)->CategoryLabel);
				confidence = classified->Item(0)->Probability;
#ifdef LOG_USE_WOFSTREAM
				std::wofstream txt_file;
				txt_file.imbue(std::locale(std::locale::empty(), new std::codecvt_utf8<wchar_t, 0x10ffff, std::generate_header>));
				txt_file.open(__LOG_FILE_NAME__, std::wofstream::out | std::wofstream::app);
				txt_file << L"- category : " << category << std::endl;
				txt_file << L"- confidence : " << confidence << std::endl;
				txt_file.close();
#endif
				if (confidence < 0.65)
					category = L"";
			}

			if (need_serialization) {
				dlib::serialize(to_cp949(cache_file)) << category << confidence;
			}

			classification_ticks.stop();
			spdlog::get("recognizer")->info("classify document : {} ({:.2f}mSec, category : {} ({:.2f}), cache : false)",
				to_cp949(file.filename().native()),
				classification_ticks.getTimeMilli(), to_cp949(category), confidence);

			return category;
		}

		inline cv::Rect
			to_rect(const FREngine::IFRRectanglePtr& rectangle)
		{
			return cv::Rect(cv::Point(rectangle->Left, rectangle->Top), cv::Size(rectangle->Width, rectangle->Height));
		}

		inline cv::Rect
			to_rect(const FREngine::ICharParamsPtr& char_param)
		{
			return cv::Rect(cv::Point(char_param->Left, char_param->Top), cv::Point(char_param->Right, char_param->Bottom));
		}

		inline cv::Rect
			to_rect(const std::pair<cv::Rect, std::wstring>& block)
		{
			return std::get<0>(block);
		}

		inline cv::Rect
			to_rect(const std::tuple<cv::Rect, std::wstring, cv::Range>& block)
		{
			return std::get<0>(block);
		}

		inline cv::Rect
			to_rect(const std::vector<std::pair<cv::Rect, std::wstring>>& blocks)
		{
			cv::Rect rect;

			if (blocks.empty())
				return rect;

			return std::accumulate(std::begin(blocks), std::end(blocks), to_rect(*std::begin(blocks)),
				[](const cv::Rect& rect, const std::pair<cv::Rect, std::wstring>& block) {
				return rect | to_rect(block);
			});
		}

		inline cv::Rect
			to_rect(const character& character)
		{
			return std::get<0>(character);
		}

		inline cv::Rect
			to_rect(const word& word)
		{
			cv::Rect rect;

			if (word.empty())
				return rect;

			return std::accumulate(std::next(std::begin(word)), std::end(word), to_rect(*std::begin(word)),
				[](const cv::Rect& rect, const character& character) {
				return rect | to_rect(character);
			});
		}

		inline cv::Rect
			to_rect(const line& line)
		{
			cv::Rect rect;

			if (line.empty())
				return rect;

			return std::accumulate(std::next(std::begin(line)), std::end(line), to_rect(*std::begin(line)),
				[](const cv::Rect& rect, const word& word) {
				return rect | to_rect(word);
			});
		}

		inline cv::Rect
			to_rect(const block& block)
		{
			cv::Rect rect;

			if (block.empty())
				return rect;

			return std::accumulate(std::next(std::begin(block)), std::end(block), to_rect(*std::begin(block)),
				[](const cv::Rect& rect, const line& line) {
				return rect | to_rect(line);
			});
		}

		inline cv::Rect
			to_rect(const std::vector<block>& blocks)
		{
			cv::Rect rect;

			if (blocks.empty())
				return rect;

			return std::accumulate(std::begin(blocks), std::end(blocks), to_rect(*std::begin(blocks)),
				[](const cv::Rect& rect, const block& block) {
				return rect | to_rect(block);
			});
		}

		inline std::wstring
			to_wstring(const std::tuple<cv::Rect, std::wstring, cv::Range>& block, bool end = false)
		{
			const auto text = std::get<1>(block);
			const auto range = std::get<2>(block);

			if (end)
				return std::wstring(std::begin(text) + range.end, std::end(text));

			return std::wstring(std::begin(text) + range.start, std::begin(text) + range.end);
		}

		inline std::wstring
			to_wstring(const std::pair<cv::Rect, std::wstring>& block)
		{
			return std::get<1>(block);
		}

		inline std::wstring
			to_wstring(const std::vector<std::pair<cv::Rect, std::wstring>>& blocks, bool separator = true)
		{
			std::wstringstream ss;
			std::for_each(std::begin(blocks), std::end(blocks), [&](const std::pair<cv::Rect, std::wstring>& block) {
				ss << to_wstring(block);
				if (separator)
					ss << L" ";
			});

			if (separator) {
				const auto str = ss.str();
				return str.substr(0, str.size() - 1);
			}

			return ss.str();
		}

		inline wchar_t
			to_wstring(const character& character)
		{
			return std::get<1>(character);
		}

		inline std::wstring
			to_wstring(const word& word)
		{
			std::wstringstream ss;
			std::for_each(std::begin(word), std::end(word), [&](const character& character) {
				ss << to_wstring(character);
			});

			return ss.str();
		}

		inline std::wstring
			to_wstring(const line& line, bool separator = true)
		{
			std::wstringstream ss;
			std::for_each(std::begin(line), std::end(line), [&](const word& word) {
				ss << to_wstring(word);
				if (separator)
					ss << L" ";
			});

			if (separator) {
				const auto str = ss.str();
				return str.substr(0, str.size() - 1);
			}

			return ss.str();
		}

		inline std::wstring
			to_wstring(const block& block, bool separator = true)
		{
			std::wstringstream ss;
			std::for_each(std::begin(block), std::end(block), [&](const line& line) {
				ss << to_wstring(line, separator);
				if (separator)
					ss << L"\n";
			});

			if (separator) {
				const auto str = ss.str();
				return str.substr(0, str.size() - 1);
			}

			return ss.str();
		}

		inline std::wstring
			to_wstring(const std::vector<block>& blocks, bool separator = true)
		{
			std::wstringstream ss;
			std::for_each(std::begin(blocks), std::end(blocks), [&](const block& line) {
				ss << to_wstring(line, separator);
				if (separator)
					ss << L"\n";
			});

			if (separator) {
				const auto str = ss.str();
				return str.substr(0, str.size() - 1);
			}

			return ss.str();
		}

		inline character
			to_character(const wchar_t ch, int left, int top, int right, int bottom)
		{
			static const std::unordered_map<wchar_t, wchar_t> CONVERSIONS{
				//특수문자
				std::make_pair(L'◆', L'-'),
				std::make_pair(L'：', L':'),
				std::make_pair(L'、', L','),
				std::make_pair(L'■', L' '),
				std::make_pair(L'­­-', L'-'),
				std::make_pair(L'―', L'-'),
				std::make_pair(0xff3b, L'['),
				std::make_pair(0xff3d, L']'),
				std::make_pair(0xff08, L'('),
				std::make_pair(0xff09, L')'),
				std::make_pair(0xfffc, L' '),
				std::make_pair(0x005e, L' '),
			};
			auto character = ch;
			if (CONVERSIONS.find(character) != CONVERSIONS.end())
				character = CONVERSIONS.at(character);

			return std::make_pair(cv::Rect(cv::Point(left, top), cv::Point(right, bottom)), character);
		}

		inline std::pair<cv::Rect, std::wstring>
			to_block(const std::tuple<cv::Rect, std::wstring, cv::Range>& block)
		{
			return std::make_pair(std::get<0>(block), std::get<1>(block));
		}

		inline std::tuple<cv::Rect, std::wstring, cv::Range>
			to_block(const std::pair<cv::Rect, std::wstring>& block)
		{
			return std::make_tuple(std::get<0>(block), std::get<1>(block), cv::Range(0, std::get<1>(block).size()));
		}

		inline cv::Size
			estimate_paper_size(const std::vector < block>& blocks)
		{
			if (blocks.empty())
				return cv::Size();

			const auto cols = to_rect(blocks).br().x;
			const auto rows = static_cast<int>(std::ceil(cols * 1.414)); // A4 paper ratio;

			return cv::Size(cols, rows);
		}

		inline bool
			combine_words(word& a, word& b)
		{
			const auto rect_a = to_rect(a);
			const auto rect_b = to_rect(b);

			if (rect_a == rect_b)
				return false;

			auto need_combine = false;

			const auto string_a = to_wstring(a);
			const auto string_b = to_wstring(b);

			const static std::set<std::wstring> SETS{
				L"성",
				L"명",
				L"영",
			};


			const static std::vector<std::set<std::wstring>> SETS_2{
				{ L"사", L"건", },
				{ L"판", L"사", },
				{ L"채", L"권", L"자", },
				{ L"제", L"3", L"채", L"무", L"자", },
			};

			auto threshold = [&]() {
				if (SETS.find(string_a) != SETS.end() &&
					SETS.find(string_b) != SETS.end())
					return std::max(rect_a.height, rect_b.height) * 5.;
				if (string_a == L"." || string_b == L".")
					return std::max(rect_a.height, rect_b.height) / 2.;
				if (string_a.size() == 1 || string_b.size() == 1)
					return std::min(rect_a.height, rect_b.height) / 2.;
				return std::min(rect_a.height, rect_b.height) / 3.;
			}();

			threshold = std::max(threshold, 15.);

			if ((rect_a & rect_b).area() > 0) {
				need_combine = true;
			}
			else if (((string_a == L"." || string_b == L".") || (std::abs(rect_a.height - rect_b.height) <= 15 && std::
				abs(rect_a.y - rect_b.y) <=
				15)) &&
				(std::abs(rect_a.br().x - rect_b.x) <= threshold || std::abs(rect_a.x - rect_b.br().x) <= threshold)) {
				need_combine = true;
			}

			if (!need_combine) {
				for (const auto& sets : SETS_2) {
					auto a_temp = rect_a.x < rect_b.x ? a : b;
					auto b_temp = rect_a.x < rect_b.x ? b : a;

					bool a_flag = true;
					bool b_flag = true;

					const auto str_a = to_wstring(a_temp);
					for (int a_i = 0; a_i < str_a.length(); ++a_i) {
						std::wstring temp(1, str_a[a_i]);

						if (sets.find(temp) == sets.end()) {
							a_flag = false;
							break;
						}
					}
					const auto str_b = to_wstring(b_temp);
					for (int b_i = 0; b_i < str_b.length(); ++b_i) {
						std::wstring temp(1, str_b[b_i]);

						if (sets.find(temp) == sets.end()) {
							b_flag = false;
							break;
						}

					}
					if (a_flag && b_flag) {
						need_combine = true;
					}
				}
			}

			if (need_combine) {
				if (rect_a.x > rect_b.x)
					std::copy(std::begin(a), std::end(a), std::back_inserter(b));
				else
					std::copy(std::begin(b), std::end(b), std::back_inserter(a));
			}

			return need_combine;
		}

		inline bool
			combine_blocks(const std::vector<block>& blocks, block& a, block& b)
		{
			const auto rect_a = to_rect(a);
			const auto rect_b = to_rect(b);

			if (rect_a == rect_b)
				return false;

			auto need_combine = false;

			const static std::set<std::wstring> SETS{
				L"성",
				L"명",
				L"영",
			};

			const static std::vector<std::set<std::wstring>> SETS_2 {
				{ L"사", L"건", },
				{ L"채", L"권", L"자", },
				{ L"채", L"무", L"자", },
				{ L"판", L"사", },
			};

			const auto string_a = to_wstring(a, false);
			const auto string_b = to_wstring(b, false);

			if ((rect_a & rect_b).area() > 0) {
				need_combine = true;
			}
			else if (std::abs(rect_a.height - rect_b.height) <= 15 && std::abs(rect_a.y - rect_b.y) <= 15) {
				if (string_a.size() == 1 && string_b.size() == 1) {
					if (SETS.find(string_a) != SETS.end() &&
						SETS.find(string_b) != SETS.end() &&
						std::abs(rect_a.br().x - rect_b.x) <= std::max(rect_a.height, rect_b.height) * 4 || std::
						abs(rect_a.x - rect_b.br().x)
						<=
						std::max(rect_a.height, rect_b.height) * 4) {
						need_combine = true;
					}
					else {
						std::vector<block> next_blocks;
						std::copy_if(std::begin(blocks), std::end(blocks), std::back_inserter(next_blocks), [&](const block& block) {
							const auto rect_block = to_rect(block);
							return (to_wstring(block, false).size() == 1 && rect_block != rect_a && std::abs(rect_a.br().x - rect_block.x)
								<=
								std::
								min(rect_a.width, rect_block.width) * 3.);
						});

						if (!next_blocks.empty()) {
							const auto nearest_block = std::min_element(std::begin(next_blocks), std::end(next_blocks),
								[&](const block& a, const block& b) {
								return to_rect(a).x - rect_a.br().x < to_rect(b).x - rect_a.br().x;
							});

							if (to_rect(*nearest_block) == rect_b)
								need_combine = true;
						}
					}
				}
				else if (std::abs(rect_a.br().x - rect_b.x) <= 15 || std::abs(rect_a.x - rect_b.br().x) <= 15) {
					need_combine = true;
				}
				if (!need_combine) {
					for (const auto& sets : SETS_2) {
						auto a_temp = rect_a.x < rect_b.x ? a : b;
						auto b_temp = rect_a.x < rect_b.x ? b : a;

						bool a_flag = true;
						bool b_flag = true;
						for (const auto& line : a_temp) {
							const auto str_a = to_wstring(line.back());
							if (str_a.size() > 1) {
								a_flag = false;
								break;
							}
							if (sets.find(str_a) == sets.end()) {
								a_flag = false;
								break;
							}
							//for (const auto& word : line) {
							//	const auto str_a = to_wstring(word);
							//	if (str_a.size() > 1) {
							//		a_flag = false;
							//		break;
							//	}
							//	if (sets.find(str_a) == sets.end()) {
							//		a_flag = false;
							//		break;
							//	}

							//}
						}

						for (const auto& line : b_temp) {
							const auto str_b = to_wstring(line.front());
							if (str_b.size() > 1) {
								b_flag = false;
								break;
							}

							if (sets.find(str_b) == sets.end()) {
								b_flag = false;
								break;
							}
							//for (const auto& word : line) {
							//	const auto str_b = to_wstring(word);
							//	if (str_b.size() > 1) {
							//		b_flag = false;
							//		break;
							//	}

							//	if (sets.find(str_b) == sets.end()) {
							//		b_flag = false;
							//		break;
							//	}
							//}
						}
						if (a_flag && b_flag) {
							need_combine = true;
						}
					}
				}
			}

			if (need_combine) {
				std::copy(std::begin(b), std::end(b), std::back_inserter(a));
			}

			return need_combine;
		}

		inline bool
			combine_lines(block& line_block, line& a, line& b)
		{
			const auto rect_a = to_rect(a);
			const auto rect_b = to_rect(b);

			if (rect_a == rect_b)
				return false;

			const auto string_a = to_wstring(a, false);
			const auto string_b = to_wstring(b, false);

			const static std::set<std::wstring> SETS{
				L"성",
				L"명",
				L"영",
			};

			const static std::vector<std::set<std::wstring>> SETS_2{
				{ L"사", L"건", },
				{ L"판", L"사", },
				{ L"채", L"권", L"자", },
				{ L"제", L"3", L"채", L"무", L"자", },
			};

			auto need_combine = false;

			if ((rect_a & rect_b).height >= std::min(rect_a.height, rect_b.height) * 0.8) {
				need_combine = true;
			}
			else if (std::abs(rect_a.height - rect_b.height) <= 15 && std::abs(rect_a.y - rect_b.y) <= 15) {
				if (string_a.size() == 1 && string_b.size() == 1) {
					if (SETS.find(string_a) != SETS.end() &&
						SETS.find(string_b) != SETS.end() &&
						std::abs(rect_a.br().x - rect_b.x) <= std::max(rect_a.height, rect_b.height) * 4 || std::
						abs(rect_a.x - rect_b.br().x)
						<=
						std::max(rect_a.height, rect_b.height) * 4) {
						need_combine = true;
					}
					else {
						std::vector<line> next_lines;
						std::copy_if(std::begin(line_block), std::end(line_block), std::back_inserter(next_lines), [&](const line& line) {
							const auto rect_line = to_rect(line);
							return (to_wstring(line, false).size() == 1 && rect_line != rect_a && std::abs(rect_a.br().x - rect_line.x) <=
								std::
								min(rect_a.width, rect_line.width) * 3.);
						});

						if (!next_lines.empty()) {
							const auto nearest_line = std::min_element(std::begin(next_lines), std::end(next_lines),
								[&](const line& a, const line& b) {
								return to_rect(a).x - rect_a.br().x < to_rect(b).x - rect_a
									.br().x;
							});

							if (to_rect(*nearest_line) == rect_b)
								need_combine = true;
						}
					}
				}
				else if (std::abs(rect_a.br().x - rect_b.x) <= 16 || std::abs(rect_a.x - rect_b.br().x) <= 16)
					need_combine = true;

				if (!need_combine) {


					for (const auto& sets : SETS_2) {
						auto a_temp = rect_a.x < rect_b.x ? a : b;
						auto b_temp = rect_a.x < rect_b.x ? b : a;

						bool a_flag = true;
						bool b_flag = true;

						const auto str_a = to_wstring(a_temp.back());
						if (str_a.size() > 1) {
							a_flag = false;
						}

						if (sets.find(str_a) == sets.end()) {
							a_flag = false;
						}


						//for (const auto& word : a) {
						//	const auto str_a = to_wstring(word);
						//	if (str_a.size() > 1) {
						//		a_flag = false;
						//		break;
						//	}

						//	if (sets.find(str_a) == sets.end()) {
						//		a_flag = false;
						//		break;
						//	}
						//}

						const auto str_b = to_wstring(b_temp.front());
						if (str_b.size() > 1) {
							b_flag = false;
						}
						if (sets.find(str_b) == sets.end()) {
							b_flag = false;
						}

						//for (const auto& word : b) {
						//	const auto str_b = to_wstring(word);
						//	if (str_b.size() > 1) {
						//		b_flag = false;
						//		break;
						//	}
						//	if (sets.find(str_b) == sets.end()) {
						//		b_flag = false;
						//		break;
						//	}
						//}
						if (a_flag && b_flag) {
							need_combine = true;
							break;
						}
					}
				}
			}



			if (need_combine) {
				std::copy(std::begin(b), std::end(b), std::back_inserter(a));
			}

			return need_combine;
		}

		inline std::pair<std::vector<block>, std::vector<std::vector<int>>>
			to_blocks(const FREngine::IPlainTextPtr& plain_text)
		{
			std::vector<block> blocks;
			std::vector<std::vector<int>> confidences;
			std::wstring text = plain_text->Text.GetBSTR();

			SAFEARRAY* page_numbers_p;
			SAFEARRAY* left_borders_p;
			SAFEARRAY* top_borders_p;
			SAFEARRAY* right_borders_p;
			SAFEARRAY* bottom_borders_p;
			SAFEARRAY* confidences_p;
			SAFEARRAY* is_suspicious_p;
			plain_text->GetCharacterData(&page_numbers_p, &left_borders_p, &top_borders_p, &right_borders_p, &bottom_borders_p, &confidences_p, &is_suspicious_p);

			SafeArrayDestroy(page_numbers_p);
			SafeArrayDestroy(is_suspicious_p);

			CComSafeArray<int> left_borders(left_borders_p);
			CComSafeArray<int> top_borders(top_borders_p);
			CComSafeArray<int> right_borders(right_borders_p);
			CComSafeArray<int> bottom_borders(bottom_borders_p);
			CComSafeArray<int> c_confidences(confidences_p);

			wchar_t word_break = L' ';
			wchar_t word_break2 = L'\t';
			wchar_t line_break = 0x2028;
			wchar_t line_break2 = 0x00AC;
			wchar_t paragraph_break = 0x2029;
			block block;
			line line;
			word word;
			std::vector<int> confidence;
			for (auto i = 0; i < text.size(); i++) {
				auto& c = text[i];
					if (c == paragraph_break) {
						if (!word.empty())
							line.emplace_back(word);
						if (!line.empty())
							block.emplace_back(line);
						if (!block.empty()) {
							blocks.emplace_back(block);
							confidences.emplace_back(confidence);
						}
						word.clear();
						line.clear();
						block.clear();
					}
					else if (c == line_break || c == line_break2) {
						if (!word.empty())
							line.emplace_back(word);
						if (!line.empty())
							block.emplace_back(line);
						word.clear();
						line.clear();
					}
					else if (c == word_break || c == word_break2) {
						if (!word.empty())
							line.emplace_back(word);
						word.clear();
					}
					else {
						word.emplace_back(to_character(c, left_borders[i], top_borders[i], right_borders[i], bottom_borders[i]));
						confidence.emplace_back(c_confidences[i]);
					}
			}
			return std::make_pair(blocks, confidences);
		}

		inline void remove_null_char(std::vector<block>& blocks) {
			for (auto& block : blocks) {
				for (auto& line : block) {
					for (auto& word : line) {
						boost::remove_erase_if(word, [](std::pair<cv::Rect, wchar_t>& character) {
							return character.second == L'\0';
						});
					}
				}
			}
		}

		inline void
			refine_blocks(std::vector<block>& blocks, bool merge_blocks = true, bool merge_lines = true, bool merge_words = true, bool divide_words = false)
		{
			remove_null_char(blocks);

			std::sort(blocks.begin(), blocks.end(), [](block& a, block& b) {
				return to_rect(a).y < to_rect(b).y;
			});

			while (merge_blocks) {
				merge_blocks = false;

				for (auto i = 0; i < blocks.size(); i++) {
					auto str = to_wstring(blocks[i], false);
					auto block = blocks[i];
					for (auto j = 1; j < blocks.size(); j++) {
						if (i < blocks.size() && j < blocks.size() && combine_blocks(blocks, blocks[i], blocks[j])) {
							blocks.erase(std::begin(blocks) + j);
							merge_blocks = true;
							j--;
						}
					}
				}
			}
			std::vector<line> new_block;
			for (auto& block : blocks) {
				for (auto& line : block) {
					new_block.emplace_back(line);
				}
			}
			blocks.clear();
			blocks.emplace_back(new_block);

			if (merge_lines) {
				for (auto& block : blocks) {
					for (auto i = 0; i < block.size(); i++) {
						auto line_i = block[i];
						auto str = to_wstring(line_i, false);
						//printf("break!\n");
						for (auto j = 1; j < block.size(); j++) {
							if (i < block.size() && j < block.size() && combine_lines(block, block[i], block[j])) {
								block.erase(std::begin(block) + j);
								j--;
							}
						}
					}
				}
			}

			for (auto& block : blocks) {
				bool merge = merge_words;
				while (merge) {
					merge = false;
					for (auto& line : block) {
						std::sort(std::begin(line), std::end(line), [](const ocr::word& a, const ocr::word& b) {
							return to_rect(a).x < to_rect(b).x;
						});

						for (auto i = 0; i < line.size(); i++) {
							for (auto j = i + 1; j < line.size(); j++) {
								if (i < line.size() && j < line.size()) {
									if (combine_words(line[i], line[j])) {
										line.erase(std::begin(line) + j);
										merge = true;
										j--;
									}
									else
										break;
								}
							}
						}
					}
				}
			}

			for (auto& block : blocks) {
				for (auto& line : block) {
					std::sort(std::begin(line), std::end(line), [](const ocr::word& a, const ocr::word& b) {
						return to_rect(a).x < to_rect(b).x;
					});

					for (auto i = 0; i < line.size(); i++) {
						auto& a = line[i];

						for (auto j = 0; j < line.size(); j++) {
							auto& b = line[j];
							if (a == b)
								continue;

							if ((to_rect(a) & to_rect(b)).area()) {
								for (auto k = 0; k < a.size(); k++) {
									auto char_a = a[k];
									boost::remove_erase_if(b, [&char_a](const character& char_b) {
										if (to_wstring(char_a) != to_wstring(char_b))
											return false;
										return (to_rect(char_a) & to_rect(char_b)).area() > to_rect(char_b).area() * 0.9;
									});
								}
							}
						}
					}

					boost::remove_erase_if(line, [](const word& word) {
						return word.empty();
					});
				}
			}

			if (divide_words) {
				for (auto b = 0; b < blocks.size(); b++) {
					auto& block = blocks[b];
					for (auto l = 0; l < block.size(); l++) {
						if (block[l].empty()) {
							continue;
						}
						std::sort(std::begin(block[l]), std::end(block[l]), [](const ocr::word& a, const ocr::word& b) {
							return to_rect(a).x < to_rect(b).x;
						});
						for (auto i = 0; i < block[l].size() - 1; i++) {
							if (block[l][i].empty() || block[l][i].size() < 3)
								continue;
							auto rect1 = to_rect(block[l][i]);
							auto rect2 = to_rect(block[l][i + 1]);
							if (rect1.br().x + rect1.width * 1.5 < rect2.x) {
								ocr::line line1, line2;
								line1.insert(line1.end(), block[l].begin(), block[l].begin() + i + 1);
								line2.insert(line2.end(), block[l].begin() + i + 1, block[l].end());
								block.emplace_back(line1);
								block.emplace_back(line2);
								block.erase(std::begin(block) + 1);
								i--;
								break;
							}
						}
					}
				}
			}

			for (auto i = 0; i < blocks.size(); i++) {
				std::sort(std::begin(blocks[i]), std::end(blocks[i]), [](const ocr::line& a, const ocr::line& b) {
					auto r1 = to_rect(a);
					auto r2 = to_rect(b);
					float threshold = ((r1.br().y - r1.tl().y) + (r2.br().y - r2.tl().y)) / 2 * 0.5;
					if (abs(r1.tl().y - r2.tl().y) < threshold)
						return (r1.tl().x < r2.tl().x);
					return r1.tl().y < r2.tl().y;
				});

				for (auto j = 0; j < blocks[i].size(); j++) {
					std::sort(std::begin(blocks[i][j]), std::end(blocks[i][j]), [](const ocr::word& a, const ocr::word& b) {
						return to_rect(a).x < to_rect(b).x;
					});
				}
			}
		}

		std::wstring getPrefix(const std::wstring& path) {
			std::ifstream file(path);

			std::string str;
			while (std::getline(file, str)) {
				if (str.find("TextLanguage") != std::string::npos) {
					if (str.find("KoreanHangul") != std::string::npos) {
						return L"ko";
					}
				}
			}
			return L"en";
		}

		std::wstring get_prefix_with_language(const std::wstring& path) {
			std::ifstream file(path);

			std::string str;
			while (std::getline(file, str)) {
				if (str.find("TextLanguage") != std::string::npos) {
					if (str.find("KoreanHangul") != std::string::npos) {
						return L"ko";
					}
				}
			}
			return L"digit";
		}

		bool needSerialization(const configuration& configuration, const std::wstring& category, const boost::filesystem::path& file, bool use_cache = __USE_CACHE__) {
			if (!use_cache) {
				return true;
			}
			const auto category_directory = fmt::format(L"{}\\{}", get_categories_directory(configuration), category);
			const auto prefix = getPrefix(boost::filesystem::path(fmt::format(L"{}\\preset.ini", category_directory)).native().c_str());

			//const auto prefix = engine->CreateRecognizerParams()->TextLanguage->BaseLanguages->Count == 1 ? L"en" : L"ko";
			const auto file_name = file.filename().native();
			auto file_name_without_ext = file_name.substr(0, file_name.size() - 4);
			file_name_without_ext = boost::regex_replace(file_name_without_ext, boost::wregex(L"_OO1"), L"");
			const auto cache_file = boost::filesystem::path(fmt::format(L"{}\\{}.{}.recognituon.cache", __CACHE_DIRCTORY__, file_name_without_ext, prefix)).native();
			return !boost::filesystem::exists(cache_file);
		}

		bool need_serialization_with_language(const configuration& configuration, const std::wstring& category, const boost::filesystem::path& file, const std::wstring language, bool use_cache = __USE_CACHE__) {
			if (!use_cache) {
				return true;
			}
			const auto category_directory = fmt::format(L"{}\\{}", get_categories_directory(configuration), category);
			const auto prefix = get_prefix_with_language(boost::filesystem::path(fmt::format(L"{}\\preset_{}.ini", category_directory, language)).native().c_str());

			//const auto prefix = engine->CreateRecognizerParams()->TextLanguage->BaseLanguages->Count == 1 ? L"en" : L"ko";
			const auto file_name = file.filename().native();
			auto file_name_without_ext = file_name.substr(0, file_name.size() - 4);
			file_name_without_ext = boost::regex_replace(file_name_without_ext, boost::wregex(L"_OO1"), L"");
			const auto cache_file = boost::filesystem::path(fmt::format(L"{}\\{}.{}.recognituon.cache", __CACHE_DIRCTORY__, file_name_without_ext, prefix)).native();
			return !boost::filesystem::exists(cache_file);
		}

		static std::tuple<std::vector<block>, std::vector<std::vector<int>>, cv::Size>
			recognize(const FREngine::IEnginePtr& engine, const configuration& configuration, const std::wstring& category, const boost::filesystem::path& file, const FREngine::IFRDocumentPtr &document,
			bool use_cache = __USE_CACHE__)
		{
			cv::TickMeter recognition_ticks;
			recognition_ticks.start();
			bool is_batch_mode = std::stoi(configuration.at(L"engine").at(L"batchmode"));
			const auto category_directory = fmt::format(L"{}\\{}", get_categories_directory(configuration), category);
			const auto prefix = getPrefix(boost::filesystem::path(fmt::format(L"{}\\preset.ini", category_directory)).native().c_str());

			//const auto prefix = engine->CreateRecognizerParams()->TextLanguage->BaseLanguages->Count == 1 ? L"en" : L"ko";
			const auto file_name = file.filename().native();
			auto file_name_without_ext = file_name.substr(0, file_name.size() - 4);
			file_name_without_ext = boost::regex_replace(file_name_without_ext, boost::wregex(L"_OO1"), L"");
			const auto cache_file = boost::filesystem::path(fmt::format(L"{}\\{}.{}.recognituon.cache", __CACHE_DIRCTORY__, file_name_without_ext, prefix)).native();

			auto need_serialization = false;
			std::vector<block> blocks;
			std::vector<std::vector<int>> confidences;
			cv::Size image_size;

			static const std::unordered_set<std::wstring> NEED_FULL_RECOGNITION{
				L"BILL OF EXCHANGE", L"CERTIFACATE", L"CERTIFICATE OF ORIGIN", L"LETTER OF CREDIT", L"WAYBILL", L"SHIPMENT ADVICE", L"BILL OF LADING", L"INSURANCE POLICY"
			};

			if (use_cache) {
				wprintf(L"cache file = %s\n", cache_file.c_str());
				if (boost::filesystem::exists(cache_file)) {
					try {
						dlib::deserialize(to_cp949(cache_file)) >> blocks >> image_size.height >> image_size.width;
						recognition_ticks.stop();
						spdlog::get("recognizer")->info("recognize document : {} ({:.2f}mSec, cache : true)",
							to_cp949(file.filename().native()),
							recognition_ticks.getTimeMilli());

						if (configuration.at(L"setting").at(L"profile") == L"trade") {
							if (NEED_FULL_RECOGNITION.find(category) == NEED_FULL_RECOGNITION.end()) {
								const auto paper_size = image_size;
								const auto height = paper_size.height;
								boost::remove_erase_if(blocks, [&height](const block& block) {
									const auto top = to_rect(block).y;
									return (top > height / 3 * 2);
								});
							}
						}

						return std::make_tuple(blocks, confidences, image_size);
					}
					catch (dlib::serialization_error&) {
					}
				}
				if (is_batch_mode || !blocks.empty())
					return std::make_tuple(blocks, confidences, image_size);
				need_serialization = true;
			}

			if (document->Pages->Item(0)->Layout != nullptr)
				document->Pages->Item(0)->Layout->Clean();

			document->Analyze(nullptr, nullptr, nullptr);

			if (configuration.at(L"setting").at(L"profile") == L"trade") {
				if (NEED_FULL_RECOGNITION.find(category) == NEED_FULL_RECOGNITION.end()) {
					const auto height = document->Pages->Item(0)->ImageDocument->GrayImage->Height;
					const auto layout_blocks = document->Pages->Item(0)->Layout->Blocks;

					for (auto i = layout_blocks->Count - 1; i >= 0; i--) {
						const auto top = layout_blocks->Item(i)->Region->BoundingRectangle->Top;
						if (top > height / 3 * 2) {
							layout_blocks->DeleteAt(i);
						}
					}
				}
			}

			document->Recognize(nullptr, nullptr);

			const auto plain_text = document->Pages->Item(0)->PlainText;

			std::tie(blocks, confidences) = to_blocks(plain_text);
#if defined(GET_IMAGE_)
			document->Pages->Item(0)->ImageDocument->GrayImage->WriteToFile(BSTR(L"TEMP_GRAY.png"), FREngine::ImageFileFormatEnum::IFF_PngGrayPng, (FREngine::IImageModification*) 0, (IUnknown*)0);
#endif

			image_size = cv::Size(document->Pages->Item(0)->ImageDocument->GrayImage->Width, document->Pages->Item(0)->ImageDocument->GrayImage->Height);
			if (need_serialization) {
				dlib::serialize(to_cp949(cache_file)) << blocks << image_size.height << image_size.width;
			}

			recognition_ticks.stop();
			spdlog::get("recognizer")->info("recognize document : {} ({:.2f}mSec, cache : false)", to_cp949(file.filename().native()),
				recognition_ticks.getTimeMilli());

			return std::make_tuple(blocks, confidences, image_size);
		}

		static std::tuple<std::vector<block>, std::vector<std::vector<int>>, cv::Size>
			recognize_with_language(const FREngine::IEnginePtr& engine, const configuration& configuration, const std::wstring& category, const boost::filesystem::path& file, const FREngine::IFRDocumentPtr &document,
			std::wstring language, bool use_cache = __USE_CACHE__)
		{
			cv::TickMeter recognition_ticks;
			recognition_ticks.start();
			bool is_batch_mode = std::stoi(configuration.at(L"engine").at(L"batchmode"));
			const auto category_directory = fmt::format(L"{}\\{}", get_categories_directory(configuration), category);
			const auto prefix = get_prefix_with_language(boost::filesystem::path(fmt::format(L"{}\\preset_{}.ini", category_directory, language)).native().c_str());

			//const auto prefix = engine->CreateRecognizerParams()->TextLanguage->BaseLanguages->Count == 1 ? L"en" : L"ko";
			const auto file_name = file.filename().native();
			auto file_name_without_ext = file_name.substr(0, file_name.size() - 4);
			file_name_without_ext = boost::regex_replace(file_name_without_ext, boost::wregex(L"_OO1"), L"");
			const auto cache_file = boost::filesystem::path(fmt::format(L"{}\\{}.{}.recognituon.cache", __CACHE_DIRCTORY__, file_name_without_ext, prefix)).native();

			auto need_serialization = false;
			std::vector<block> blocks;
			std::vector<std::vector<int>> confidences;
			cv::Size image_size;

			static const std::unordered_set<std::wstring> NEED_FULL_RECOGNITION{
				L"BILL OF EXCHANGE", L"CERTIFACATE", L"CERTIFICATE OF ORIGIN", L"LETTER OF CREDIT", L"WAYBILL", L"SHIPMENT ADVICE", L"BILL OF LADING", L"INSURANCE POLICY"
			};

			if (use_cache) {
				wprintf(L"cache file = %s\n", cache_file.c_str());
				if (boost::filesystem::exists(cache_file)) {
					try {
						dlib::deserialize(to_cp949(cache_file)) >> blocks >> image_size.height >> image_size.width;
						recognition_ticks.stop();
						spdlog::get("recognizer")->info("recognize document : {} ({:.2f}mSec, cache : true)",
							to_cp949(file.filename().native()),
							recognition_ticks.getTimeMilli());

						if (configuration.at(L"setting").at(L"profile") == L"trade") {
							if (NEED_FULL_RECOGNITION.find(category) == NEED_FULL_RECOGNITION.end()) {
								const auto paper_size = image_size;
								const auto height = paper_size.height;
								boost::remove_erase_if(blocks, [&height](const block& block) {
									const auto top = to_rect(block).y;
									return (top > height / 3 * 2);
								});
							}
						}

						return std::make_tuple(blocks, confidences, image_size);
					}
					catch (dlib::serialization_error&) {
					}
				}
				if (is_batch_mode || !blocks.empty())
					return std::make_tuple(blocks, confidences, image_size);
				need_serialization = true;
			}

			if (document->Pages->Item(0)->Layout != nullptr)
				document->Pages->Item(0)->Layout->Clean();

			document->Analyze(nullptr, nullptr, nullptr);

			if (configuration.at(L"setting").at(L"profile") == L"trade") {
				if (NEED_FULL_RECOGNITION.find(category) == NEED_FULL_RECOGNITION.end()) {
					const auto height = document->Pages->Item(0)->ImageDocument->GrayImage->Height;
					const auto layout_blocks = document->Pages->Item(0)->Layout->Blocks;

					for (auto i = layout_blocks->Count - 1; i >= 0; i--) {
						const auto top = layout_blocks->Item(i)->Region->BoundingRectangle->Top;
						if (top > height / 3 * 2) {
							layout_blocks->DeleteAt(i);
						}
					}
				}
			}

			document->Recognize(nullptr, nullptr);

			const auto plain_text = document->Pages->Item(0)->PlainText;

			std::tie(blocks, confidences) = to_blocks(plain_text);
			image_size = cv::Size(document->Pages->Item(0)->ImageDocument->GrayImage->Width, document->Pages->Item(0)->ImageDocument->GrayImage->Height);
			if (need_serialization) {
				dlib::serialize(to_cp949(cache_file)) << blocks << image_size.height << image_size.width;
			}

			recognition_ticks.stop();
			spdlog::get("recognizer")->info("recognize document : {} ({:.2f}mSec, cache : false)", to_cp949(file.filename().native()),
				recognition_ticks.getTimeMilli());

			return std::make_tuple(blocks, confidences, image_size);
		}

		inline std::tuple<std::vector<block>, std::vector<std::vector<int>>, cv::Size>
			recognize_document(const FREngine::IEnginePtr& engine, const configuration& configuration, const std::wstring& category,
			const boost::filesystem::path& file, const FREngine::IFRDocumentPtr& document, bool merge_blocks = true,
			bool merge_lines = true, bool merge_words = true, bool divide_words = false,
			bool use_cache = __USE_CACHE__)
		{
			const auto category_directory = fmt::format(L"{}\\{}", get_categories_directory(configuration), category);
			bool need_serialization = needSerialization(configuration, category, file);
#ifndef __USE_ENGINE__
			if (need_serialization) {
				return std::tie(std::vector<block>(), std::vector<std::vector<int>>(), cv::Size(0, 0));
			}
#endif
			if (need_serialization) {
				engine->CleanRecognizerSession();
				engine->LoadPredefinedProfile(configuration.at(L"engine").at(L"profile").c_str());
				engine->LoadProfile(boost::filesystem::path(fmt::format(L"{}\\preset.ini", category_directory)).native().c_str());
			}

			std::vector<block> blocks;
			std::vector<std::vector<int>> confidences;
			cv::Size image_size;
			std::tie(blocks, confidences, image_size) = recognize(engine, configuration, category, file, document, use_cache);
			refine_blocks(blocks, merge_blocks, merge_lines, merge_words, divide_words);

			return std::make_tuple(blocks, confidences, image_size);
		}

		inline std::tuple<std::vector<block>, std::vector<std::vector<int>>, cv::Size>
			recognize_document_with_language(const FREngine::IEnginePtr& engine, const configuration& configuration, const std::wstring& category,
			const boost::filesystem::path& file, const FREngine::IFRDocumentPtr& document, std::wstring language, bool merge_blocks = true,
			bool merge_lines = true, bool merge_words = true, bool divide_words = false,
			bool use_cache = __USE_CACHE__)
		{
			const auto category_directory = fmt::format(L"{}\\{}", get_categories_directory(configuration), category);
			bool need_serialization = need_serialization_with_language(configuration, category, file, language);
#ifndef __USE_ENGINE__
			if (need_serialization) {
				return std::tie(std::vector<block>(), std::vector<std::vector<int>>(), cv::Size(0, 0));
			}
#endif
			if (need_serialization) {
				engine->CleanRecognizerSession();
				engine->LoadPredefinedProfile(configuration.at(L"engine").at(L"profile").c_str());
				engine->LoadProfile(boost::filesystem::path(fmt::format(L"{}\\preset_{}.ini", category_directory, language)).native().c_str());
			}
			std::vector<block> blocks;
			std::vector<std::vector<int>> confidences;
			cv::Size image_size;
			std::tie(blocks, confidences, image_size) = recognize_with_language(engine, configuration, category, file, document, language, use_cache);
			refine_blocks(blocks, merge_blocks, merge_lines, merge_words, divide_words);

			return std::make_tuple(blocks, confidences, image_size);
		}

		static std::tuple<std::vector<block>, std::vector<std::vector<int>>, cv::Size>
			recognize(const FREngine::IEnginePtr& engine, const configuration& configuration, const FREngine::IFRDocumentPtr &document)
		{
			cv::TickMeter recognition_ticks;
			recognition_ticks.start();
			std::vector<block> blocks;
			std::vector<std::vector<int>> confidences;

			if (document->Pages->Item(0)->Layout != nullptr)
				document->Pages->Item(0)->Layout->Clean();

			auto recognizer_params = engine->CreateRecognizerParams();
			if (configuration.at(L"engine").find(L"languages") != configuration.at(L"engine").end())
				recognizer_params->SetPredefinedTextLanguage(configuration.at(L"engine").at(L"languages").c_str());

			document->Analyze(nullptr, nullptr, recognizer_params);
			document->Recognize(nullptr, nullptr);

			const auto plain_text = document->Pages->Item(0)->PlainText;
			cv::Size image_size(document->Pages->Item(0)->ImageDocument->GrayImage->Width, document->Pages->Item(0)->ImageDocument->GrayImage->Height);
			std::tie(blocks, confidences) = to_blocks(plain_text);

			recognition_ticks.stop();
			spdlog::get("recognizer")->info("recognize document : ({:.2f}mSec)", recognition_ticks.getTimeMilli());

			return std::make_tuple(blocks, confidences, image_size);
		}

		inline std::tuple<std::vector<block>, std::vector<std::vector<int>>, cv::Size>
			recognize_document(const FREngine::IEnginePtr& engine, const configuration& configuration, const FREngine::IFRDocumentPtr& document,
			bool merge_blocks = true, bool merge_lines = true, bool merge_words = true)
		{
			engine->CleanRecognizerSession();
			engine->LoadPredefinedProfile(configuration.at(L"engine").at(L"profile").c_str());

			auto profile_path = boost::filesystem::path("preset.ini");
			if (!boost::filesystem::exists(profile_path))
				profile_path = boost::filesystem::path(fmt::format(L"{}\\bin\\preset.ini", get_install_path()));

			engine->LoadProfile(boost::filesystem::absolute(profile_path).native().c_str());

			std::vector<block> blocks;
			std::vector<std::vector<int>> confidences;
			cv::Size image_size;
			std::tie(blocks, confidences, image_size) = recognize(engine, configuration, document);
			refine_blocks(blocks, merge_blocks, merge_lines, merge_words);

			return std::make_tuple(blocks, confidences, image_size);
		}


		inline std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>
			search_fields2(const std::wstring& category, const std::unordered_map<std::wstring, std::vector<std::wstring>>& keywords,
			const std::vector<block>& blocks)
		{
			std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>> searched_fields;

			std::vector<std::wstring> keyword_list;

			std::vector<std::wstring> keys(keywords.size());

			std::transform(keywords.begin(), keywords.end(), keys.begin(), [](const std::pair<std::wstring, std::vector<std::wstring>>& keyword){
				return keyword.first;
			});

			for (const auto& key : keys) {
				const auto& keyword_list = keywords.at(key);
				aho_corasick::wtrie trie;
				trie.case_insensitive().remove_overlaps().allow_space();
				build_trie(trie, keyword_list);

				const auto dictionary = build_spell_dictionary(keyword_list);

				searched_fields.emplace(std::make_pair(key, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>()));

				for (const auto& block : blocks) {
					for (const auto& line : block) {
						const auto line_rect = to_rect(line);
						auto text = to_wstring(line);

						std::vector<std::wstring> words;

						boost::algorithm::split(words, text, boost::is_any_of(L"\n\t "), boost::token_compress_on);

						for (auto& word : words) {
							const auto suggested = dictionary->Correct(word);
							if (!suggested.empty() && suggested[0].distance <= 1)
								word = suggested[0].term;
						}

						text = boost::algorithm::join(words, L" ");

						const auto suggested = dictionary->Correct(text);
						if (!suggested.empty() && suggested[0].distance <= 1)
							text = suggested[0].term;

						if (category == L"XIAMETER") {
							const auto before = text;
							text = boost::regex_replace(text, boost::wregex(L"고객.{1,2}품"), L"고객물품");
							//text = boost::regex_replace(text, boost::wregex(L"검사 결과"), L"검사결과");
						}

						auto matches = trie.parse_text(text);

						if (matches.empty()) {
							boost::erase_all(text, L" ");
							const auto suggested = dictionary->Correct(text);
							if (!suggested.empty() && suggested[0].distance <= 1)
								text = suggested[0].term;
							matches = trie.parse_text(text);
						}

						for (const auto& match : matches) {
							const auto field_name = key;
							const auto last_rect = searched_fields[field_name].empty()
								? cv::Rect()
								: std::get<0>(searched_fields[field_name].back());

							if (last_rect != line_rect)
								searched_fields[field_name].emplace_back(std::make_tuple(line_rect, text,
								cv::Range(match.get_start(), match.get_end() + 1)));
						}
					}
				}


			}

			return searched_fields;
		}

		inline std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>
			search_fields(const std::wstring& category, const std::unordered_map<std::wstring, std::vector<std::wstring>>& keywords,
			const std::vector<block>& blocks)
		{
			if (category == L"XIAMETER") {
				return search_fields2(category, keywords, blocks); // 검출 이슈 키워드('검사방법', '검사결과') 라인 ('검사 방법 검사 결과 단위 .....') '검사결과' 검출 안됨
			}

			std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>> searched_fields;
			std::unordered_map<std::wstring, std::wstring> inverted_keywords;
			std::vector<std::wstring> keyword_list;

			std::for_each(std::begin(keywords), std::end(keywords), [&](const std::pair<std::wstring, std::vector<std::wstring>>& keyword) {
				searched_fields.emplace(std::make_pair(std::get<0>(keyword), std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>()));

				for (const auto& word : std::get<1>(keyword)) {
					auto keyword_word = std::wstring(word);
					boost::erase_all(keyword_word, L" ");

					inverted_keywords.emplace(std::make_pair(keyword_word, std::get<0>(keyword)));
					keyword_list.emplace_back(word);
				}
			});

			aho_corasick::wtrie trie;
			trie.case_insensitive().remove_overlaps().allow_space();
			build_trie(trie, keyword_list);

			const auto dictionary = build_spell_dictionary(keyword_list);

			for (const auto& block : blocks) {
				for (const auto& line : block) {
					const auto line_rect = to_rect(line);
					auto text = to_wstring(line);

					std::vector<std::wstring> words;
					if (category == L"Debt Repayment Inquiry") {
						boost::algorithm::split(words, text, boost::is_any_of(L"\n\t:"), boost::token_compress_on);
					}
					else {
						boost::algorithm::split(words, text, boost::is_any_of(L"\n\t "), boost::token_compress_on);
					}

					for (auto& word : words) {
						if (boost::iequals(word, "shipped"))
							continue;
						else if (category == L"DONGSUNG" && (boost::iequals(word, L"제조") || boost::iequals(word, L"제품")))
							continue;

						const auto suggested = dictionary->Correct(word);
						if (!suggested.empty() && suggested[0].distance <= 1)
							word = suggested[0].term;
					}

					text = boost::algorithm::join(words, L" ");

					const auto suggested = dictionary->Correct(text);
					if (!suggested.empty() && suggested[0].distance <= 1)
						text = suggested[0].term;

					if (category == L"Financial Transaction Information Request"
						|| category == L"Bottom Contact Info page") {
						text = boost::regex_replace(text, boost::wregex(L"요구자 기관명"), L"요구기관명");
						text = boost::regex_replace(text, boost::wregex(L"요구[기|자]{1} 성명"), L"요구기관명");
						text = boost::regex_replace(text, boost::wregex(L"[문운]{1}서[번민빈인]{1}호"), L"문서번호");
						text = boost::regex_replace(text, boost::wregex(L"[전진견]{1}(화|하|와||호F|최){1}[^-가-힣A-Za-z0-9 ]{0,2}[번민빈인]{1}호"), L"전화번호");
						text = boost::regex_replace(text, boost::wregex(L"Te1"), L"TEL");
						text = boost::regex_replace(text, boost::wregex(L"[Tt]{1}[.,]{1}"), L"t ");
						text = boost::regex_replace(text, boost::wregex(L"[Ff]{1}[.,]{1}"), L"f.");
						text = boost::regex_replace(text, boost::wregex(L"[팩백택액]{1}스"), L"팩스");
						text = boost::regex_replace(text, boost::wregex(L"[EeFfP]{1}[Aa]{1}[Xx]{1}"), L"FAX");
						text = boost::regex_replace(text, boost::wregex(L"유여!기간"), L"유예기간");
						if (text.find(L"전화") != std::wstring::npos && text.find(L"연락처") != std::wstring::npos) {
							text = boost::regex_replace(text, boost::wregex(L"연락처"), L"");
						}
					}
					else if (category == L"Debt Repayment Inquiry") {
						text = boost::regex_replace(text, boost::wregex(L"[전진견]{1}(화|하|와|호F|최){1}[^-가-힣A-Za-z0-9 ]{0,2}[번민빈인]{1}호"), L"전화번호");
						text = boost::regex_replace(text, boost::wregex(L"Te1"), L"TEL");
						text = boost::regex_replace(text, boost::wregex(L"[팩백택액]{1}스"), L"팩스");
						text = boost::regex_replace(text, boost::wregex(L"[EeFfP]{1}[Aa]{1}[Xx]{1}"), L"FAX");
					}
					else if (category == L"Confiscation Search Verification Warrent") {
						text = boost::regex_replace(text, boost::wregex(L"[죄|조1|조!]{1} ?[명|정]{1}"), L"죄명");
						text = boost::regex_replace(text, boost::wregex(L"[영|잉]{1}장[번민빈인]{1}호"), L"영장번호");
					}
					else if (category == L"IDCard Of A PO With Contact Info") {
						text = boost::regex_replace(text, boost::wregex(L"[전진견]{1}[ ]{1}(화|하|학|와|호F|최|호\\.){1}"), L"전화");
						text = boost::regex_replace(text, boost::wregex(L"Te1"), L"TEL");
						text = boost::regex_replace(text, boost::wregex(L"[팩백택맥맥]{1}[ ]{1}스"), L"팩스");
						text = boost::regex_replace(text, boost::wregex(L"사[무|우]{1}[실|설]{1}"), L"사무실");
					}
					else if (category == L"Cover Page Top Down Info") {
						text = boost::regex_replace(text, boost::wregex(L"[전진견]{1}(화|하|와||호F|최){1}[^-가-힣A-Za-z0-9 ]{0,2}[번민빈인]{1}호"), L"전화번호");
						text = boost::regex_replace(text, boost::wregex(L"[팩백택액]{1}스"), L"팩스");
					}
					else if (category == L"POWERTECH") {
						text = boost::regex_replace(text, boost::wregex(L"[a-z가힣]nit", boost::wregex::icase), L"unit");
						text = boost::regex_replace(text, boost::wregex(L"[Tl]est ?R(.*)", boost::wregex::icase), L"Test Result");
					}
					else if (category == L"DONGSUNG") {
						text = boost::regex_replace(text, boost::wregex(L"[제재세새] ?[품풍폼퐁] ?[명몀영염]"), L"제품명");
						text = boost::regex_replace(text, boost::wregex(L"단우[1,i,I,l]"), L"단위"); // 수정필요
					}
					else if (category == L"UNID") {
						text = boost::regex_replace(text, boost::wregex(L"[풍품폼퐁] ?[멍몀멈]"), L"품명");
					}
					else if (category == L"TEASUNG CHEMICAL") {
						text = boost::regex_replace(text, boost::wregex(L"^Prod니ct Name"), L"Product Name"); // 수정 필요
						text = boost::regex_replace(text, boost::wregex(L"^Product Nome"), L"Product Name");
					}
					else if (category == L"SKGC") {
						text = boost::regex_replace(text, boost::wregex(L"번흐"), L"번호"); // 수정 필요
						text = boost::regex_replace(text, boost::wregex(L"로.번호"), L"로트번호"); // 수정 필요
						text = boost::regex_replace(text, boost::wregex(L"^시험결고[[F느]"), L"시험결과");
					}
					else if (category == L"BASF_B") {
						text = boost::regex_replace(text, boost::wregex(L"TES(.*)RESULTS", std::wregex::icase), L"TESTRESULTS");
					}
					else if (category == L"DUKSAN") {
						text = boost::regex_replace(text, boost::wregex(L"M[if]g", std::wregex::icase), L"Mfg");
					}
					else if (category == L"WEIHAI") {
						text = boost::regex_replace(text, boost::wregex(L"[(]产品名称[)]{0,1}"), L"(产品名称)");
						text = boost::regex_replace(text, boost::wregex(L"生.{1,2}期"), L"生产日期");
					}
					else if (category == L"ALBEMARLE") {
						text = boost::regex_replace(text, boost::wregex(L"Charseteristic", std::wregex::icase), L"characteristic");
					}
					else if (category == L"CHIMEI") {
						text = boost::regex_replace(text, boost::wregex(L"Lol No.", std::wregex::icase), L"Lot No.");
					}
					else if (category == L"ACCOUNT") {
						text = boost::regex_replace(text, boost::wregex(L"국[민인][은온]"), L"국민은");
						text = boost::regex_replace(text, boost::wregex(L"[농눙][협현업럽]"), L"농협");
						text = boost::regex_replace(text, boost::wregex(L"롯더정보"), L"롯데정보");
						text = boost::regex_replace(text, boost::wregex(L"뉴스컴되니"), L"뉴스컴퍼니");
					}
					else if (category == L"BILL OF LADING") {
						text = boost::regex_replace(text, boost::wregex(L"HOFC"), L"HDFC");
					}

					auto matches = trie.parse_text(text);

					if (matches.empty()) {
						boost::erase_all(text, L" ");
						const auto suggested = dictionary->Correct(text);
						if (!suggested.empty() && suggested[0].distance <= 1)
							text = suggested[0].term;
						matches = trie.parse_text(text);
					}

					for (const auto& match : matches) {
						const auto field_name = inverted_keywords[match.get_keyword()];
						const auto last_rect = searched_fields[field_name].empty()
							? cv::Rect()
							: std::get<0>(searched_fields[field_name].back());

						if (last_rect != line_rect)
							searched_fields[field_name].emplace_back(std::make_tuple(line_rect, text,
							cv::Range(match.get_start(), match.get_end() + 1)));
					}
				}
			}

			return searched_fields;
		}




		inline std::vector<std::pair<cv::Rect, std::wstring>>
			find_horizontal_words(const std::tuple<cv::Rect, std::wstring, cv::Range>& basis, const std::vector<block>& blocks, const float hitting_threshold = 0.5, const float expanded_ratio = 0.0, bool expand_both_sides = false)
		{
			std::vector<std::pair<cv::Rect, std::wstring>> found_lines;
			auto basis_rect = cv::Rect(std::get<0>(basis));
			basis_rect.x = 0;
			basis_rect.width = std::numeric_limits<int>::max();
			int expanded_height = basis_rect.height * expanded_ratio;;
			basis_rect.height += expanded_height;
			if (expand_both_sides) {
				basis_rect.y -= expanded_height / 2;
			}

			for (const auto& block : blocks) {
				for (const auto& line : block) {
					for (const auto& word : line) {
						auto word_rect = to_rect(word);

						if (word_rect == std::get<0>(basis))
							continue;

						const auto collision = basis_rect & word_rect;

						if (collision.height >= std::min(basis_rect.height, word_rect.height) * hitting_threshold) {
							found_lines.emplace_back(std::make_pair(to_rect(word), to_wstring(word)));
						}
					}

				}
			}

			return found_lines;
		}

		inline std::vector<std::pair<cv::Rect, std::wstring>>
			find_area_lines(const std::tuple<cv::Rect, std::wstring, cv::Range>& basis, const std::vector<block>& blocks, const float hitting_threshold = 0.5) {

			std::vector<std::pair<cv::Rect, std::wstring>> found_lines;
			auto basis_rect = cv::Rect(std::get<0>(basis));
			int expanded_height = basis_rect.height;;
			basis_rect.height += expanded_height;

			for (const auto& block : blocks) {
				for (const auto& line : block) {
					auto line_rect = to_rect(line);

					if (line_rect == std::get<0>(basis))
						continue;

					const auto collision = basis_rect & line_rect;

					if (collision.height >= std::min(basis_rect.height, line_rect.height) * hitting_threshold) {
						found_lines.emplace_back(std::make_pair(to_rect(line), to_wstring(line)));
					}
				}
			}

			return found_lines;
		}

		inline std::vector<std::pair<cv::Rect, std::wstring>>
			find_horizontal_lines(const std::tuple<cv::Rect, std::wstring, cv::Range>& basis, const std::vector<block>& blocks, const float hitting_threshold = 0.5, const float expanded_ratio = 0.0, bool expand_both_sides = false)
		{
			std::vector<std::pair<cv::Rect, std::wstring>> found_lines;
			auto basis_rect = cv::Rect(std::get<0>(basis));
			basis_rect.x = 0;
			basis_rect.width = std::numeric_limits<int>::max();
			int expanded_height = basis_rect.height * expanded_ratio;;
			basis_rect.height += expanded_height;
			if (expand_both_sides) {
				basis_rect.y -= expanded_height / 2;
			}

			for (const auto& block : blocks) {
				for (const auto& line : block) {
					auto line_rect = to_rect(line);

					if (line_rect == std::get<0>(basis))
						continue;

					const auto collision = basis_rect & line_rect;

					if (collision.height >= std::min(basis_rect.height, line_rect.height) * hitting_threshold) {
						found_lines.emplace_back(std::make_pair(to_rect(line), to_wstring(line)));
					}
				}
			}

			return found_lines;
		}

		inline std::vector<std::pair<cv::Rect, std::wstring>>
			find_horizontal_lines(const std::tuple<cv::Rect, std::wstring, cv::Range>& basis,
			const std::vector<std::pair<cv::Rect, std::wstring>>& lines, const float hitting_threshold = 0.5, const float expanded_ratio = 0.0, bool expand_both_sides = false)
		{
			auto basis_rect = cv::Rect(std::get<0>(basis));
			basis_rect.x = 0;
			basis_rect.width = std::numeric_limits<int>::max();
			int expanded_height = basis_rect.height * expanded_ratio;;
			basis_rect.height += expanded_height;
			if (expand_both_sides) {
				basis_rect.y -= expanded_height / 2;
			}

			std::vector<std::pair<cv::Rect, std::wstring>> found_lines;
			std::copy_if(std::begin(lines), std::end(lines), std::back_inserter(found_lines),
				[&](const std::pair<cv::Rect, std::wstring>& line) {
				auto line_rect = to_rect(line);

				if (line_rect == std::get<0>(basis))
					return false;

				const auto collision = basis_rect & line_rect;

				return (collision.height >= std::min(basis_rect.height, line_rect.height) * hitting_threshold);
			});

			return found_lines;
		}

		inline std::vector<std::pair<cv::Rect, std::wstring>>
			find_right_words(const std::tuple<cv::Rect, std::wstring, cv::Range>& basis, const std::vector<block>& blocks, const float hitting_threshold = 0.5, const float expanded_ratio = 0.0, const float cutting_ratio = 100.0, bool expand_both_sides = false)
		{
			std::vector<std::pair<cv::Rect, std::wstring>> found_lines;

			const auto horizontal_lines = find_horizontal_words(basis, blocks, hitting_threshold, expanded_ratio, expand_both_sides);

			for (const auto& line : horizontal_lines) {
				const auto line_rect = std::get<0>(line);
				if (std::get<0>(basis).br().x < line_rect.x)
					found_lines.emplace_back(line);
			}

			boost::remove_erase_if(found_lines, [&basis, &cutting_ratio](const std::pair<cv::Rect, std::wstring>& block) {
				const auto cut = to_rect(basis).br().x + to_rect(basis).width * cutting_ratio;
				return to_rect(block).x > cut;
			});

			std::sort(std::begin(found_lines), std::end(found_lines),
				[](const std::pair<cv::Rect, std::wstring>& a, const std::pair<cv::Rect, std::wstring>& b) {
				return std::get<0>(a).x < std::get<0>(b).x;
			});

			return found_lines;
		}

		inline std::vector<std::pair<cv::Rect, std::wstring>>
			find_nearest_right_word(const std::tuple<cv::Rect, std::wstring, cv::Range>& basis, const std::vector<block>& blocks, const float hitting_threshold = 0.5, const float expanded_ratio = 0.0, const float cutting_ratio = 100.0, bool expand_both_sides = false)
		{
			const auto found_lines = find_right_words(basis, blocks, hitting_threshold, expanded_ratio, cutting_ratio, expand_both_sides);

			if (found_lines.empty())
				return std::vector<std::pair<cv::Rect, std::wstring>>();

			return std::vector<std::pair<cv::Rect, std::wstring>>{found_lines[0]};
		}


		inline std::vector<std::pair<cv::Rect, std::wstring>>
			find_vertical_characters(const std::tuple<cv::Rect, std::wstring, cv::Range>& basis, const std::vector<block>& blocks, const float hitting_threshold = 0.5, const float expanded_ratio = 0.0, bool expand_both_sides = false, const bool restict_right = false, const float restict_ratio = 0)
		{
			std::vector<std::pair<cv::Rect, std::wstring>> found_lines;
			auto basis_rect = cv::Rect(std::get<0>(basis));
			basis_rect.y = 0;
			basis_rect.height = std::numeric_limits<int>::max();
			int expanded_width = basis_rect.width * expanded_ratio;
			basis_rect.width += expanded_width;
			if (expand_both_sides) {
				basis_rect.x -= expanded_width / 2;
			}

			int restrict_right_x = 10000;
			cv::Rect restict_rect(0, 0, 0, 0);

			if (restict_right) {
				const auto found_words = find_nearest_right_word(basis, blocks, 0.5, 0.0, 100, false);
				if (found_words.size() > 0) {
					restict_rect = found_words[0].first;
					restrict_right_x = found_words[0].first.x - found_words[0].first.width * restict_ratio;
					basis_rect.width = std::min(basis_rect.width, restrict_right_x - basis_rect.x);
				}
			}

			for (const auto& block : blocks) {
				for (const auto& line : block) {
					auto line_str = to_wstring(line);
					std::vector<word> temp_words;
					for (const auto& word : line) {
						auto word_str = to_wstring(word);
						auto word_rect = to_rect(word);
						auto self_rect = std::get<0>(basis) & word_rect;

						if (word_rect == std::get<0>(basis) || self_rect.width > std::min(basis_rect.width, word_rect.width) * 0.9)
							continue;

						const auto collision = basis_rect & word_rect;

						if (collision.width >= std::min(basis_rect.width, word_rect.width) * hitting_threshold) {
							if (restict_rect.width > 0) {
								restict_rect.y = 0;
								restict_rect.height = std::numeric_limits<int>::max();
								cv::Rect check = word_rect & restict_rect;
								if (check.width > collision.width)  {
									continue;
								}
							}
							auto character_list = word;
							boost::remove_erase_if(character_list, [&basis_rect](const std::pair<cv::Rect, wchar_t>& character) {
								int left = basis_rect.x;
								int right = basis_rect.width + left;
								if (character.first.x + character.first.width < left || right < character.first.x) {
									return true;
								}
								return false;
							});
							if (temp_words.size() > 0) {
								cv::Rect pre_rect = to_rect(temp_words.back());
								if (pre_rect.y + pre_rect.height < word_rect.y || std::abs(pre_rect.x - word_rect.x) < 3) {
									found_lines.emplace_back(std::make_pair(to_rect(temp_words), to_wstring(temp_words)));
									temp_words.clear();
								}
							}
							temp_words.push_back(character_list);
						}
					}
					if (!temp_words.empty()) {
						found_lines.emplace_back(std::make_pair(to_rect(temp_words), to_wstring(temp_words)));
					}
				}
			}

			return found_lines;
		}

		inline std::vector<std::pair<cv::Rect, std::wstring>>
			find_vertical_words(const std::tuple<cv::Rect, std::wstring, cv::Range>& basis, const std::vector<block>& blocks, const float hitting_threshold = 0.5, const float expanded_ratio = 0.0, bool expand_both_sides = false, const bool restict_right = false, const float restict_ratio = 0)
		{
			std::vector<std::pair<cv::Rect, std::wstring>> found_lines;
			auto basis_rect = cv::Rect(std::get<0>(basis));
			basis_rect.y = 0;
			basis_rect.height = std::numeric_limits<int>::max();
			int expanded_width = basis_rect.width * expanded_ratio;
			basis_rect.width += expanded_width;
			if (expand_both_sides) {
				basis_rect.x -= expanded_width / 2;
			}

			int restrict_right_x = 10000;
			cv::Rect restict_rect(0, 0, 0, 0);

			if (restict_right) {
				const auto found_words = find_nearest_right_word(basis, blocks, 0.5, 0.0, 100, false);
				if (found_words.size() > 0) {
					restict_rect = found_words[0].first;
					restrict_right_x = found_words[0].first.x - found_words[0].first.width * restict_ratio;
					basis_rect.width = std::min(basis_rect.width, restrict_right_x - basis_rect.x);
				}
			}

			for (const auto& block : blocks) {
				for (const auto& line : block) {
					auto line_str = to_wstring(line);
					std::vector<word> temp_words;
					for (const auto& word : line) {
						auto word_str = to_wstring(word);
						auto word_rect = to_rect(word);
						auto self_rect = std::get<0>(basis) & word_rect;

						if (word_rect == std::get<0>(basis) || self_rect.width > std::min(basis_rect.width, word_rect.width) * 0.9)
							continue;

						const auto collision = basis_rect & word_rect;

						if (collision.width >= std::min(basis_rect.width, word_rect.width) * hitting_threshold) {
							if (restict_rect.width > 0) {
								restict_rect.y = 0;
								restict_rect.height = std::numeric_limits<int>::max();
								cv::Rect check = word_rect & restict_rect;
								if (check.width > collision.width)  {
									continue;
								}
							}
							if (temp_words.size() > 0) {
								cv::Rect pre_rect = to_rect(temp_words.back());
								if (pre_rect.y + pre_rect.height < word_rect.y || std::abs(pre_rect.x - word_rect.x) < 3) {
									found_lines.emplace_back(std::make_pair(to_rect(temp_words), to_wstring(temp_words)));
									temp_words.clear();
								}
							}
							temp_words.push_back(word);
						}
					}
					if (!temp_words.empty()) {
						found_lines.emplace_back(std::make_pair(to_rect(temp_words), to_wstring(temp_words)));
					}
				}
			}

			return found_lines;
		}


		inline std::vector<std::pair<cv::Rect, std::wstring>>
			find_vertical_lines(const std::tuple<cv::Rect, std::wstring, cv::Range>& basis, const std::vector<block>& blocks, const float hitting_threshold = 0.5, const float expanded_ratio = 0.0, bool expand_both_sides = false)
		{
			std::vector<std::pair<cv::Rect, std::wstring>> found_lines;
			auto basis_rect = cv::Rect(std::get<0>(basis));
			basis_rect.y = 0;
			basis_rect.height = std::numeric_limits<int>::max();
			int expanded_width = basis_rect.width * expanded_ratio;
			basis_rect.width += expanded_width;
			if (expand_both_sides) {
				basis_rect.x -= expanded_width / 2;
			}

			for (const auto& block : blocks) {
				for (const auto& line : block) {
					auto line_rect = to_rect(line);

					if (line_rect == std::get<0>(basis))
						continue;

					const auto collision = basis_rect & line_rect;

					if (collision.width >= std::min(basis_rect.width, line_rect.width) * hitting_threshold) {
						found_lines.emplace_back(std::make_pair(to_rect(line), to_wstring(line)));
					}
				}
			}

			return found_lines;
		}


		inline std::vector<std::pair<cv::Rect, std::wstring>>
			find_right_lines(const std::tuple<cv::Rect, std::wstring, cv::Range>& basis, const std::vector<block>& blocks, const float hitting_threshold = 0.5, const float expanded_ratio = 0.0, const float cutting_ratio = 100.0, bool expand_both_sides = false)
		{
			std::vector<std::pair<cv::Rect, std::wstring>> found_lines;

			const auto horizontal_lines = find_horizontal_lines(basis, blocks, hitting_threshold, expanded_ratio, expand_both_sides);

			for (const auto& line : horizontal_lines) {
				const auto line_rect = std::get<0>(line);
				if (std::get<0>(basis).br().x < line_rect.x)
					found_lines.emplace_back(line);
			}

			boost::remove_erase_if(found_lines, [&basis, &cutting_ratio](const std::pair<cv::Rect, std::wstring>& block) {
				const auto cut = to_rect(basis).br().x + to_rect(basis).width * cutting_ratio;
				return to_rect(block).x > cut;
			});

			std::sort(std::begin(found_lines), std::end(found_lines),
				[](const std::pair<cv::Rect, std::wstring>& a, const std::pair<cv::Rect, std::wstring>& b) {
				return std::get<0>(a).x < std::get<0>(b).x;
			});

			return found_lines;
		}

		inline bool filter_search(const std::tuple<cv::Rect, std::wstring, cv::Range>& basis, const std::vector<block>& blocks) {
			if (std::get<2>(basis).start > 1 || std::get<1>(basis).length() > 6) {
				return true;
			}
			return false;
		}

		inline bool filter_search_2(const std::tuple<cv::Rect, std::wstring, cv::Range>& basis, const std::vector<block>& blocks) {
			if (std::get<2>(basis).start > 1) {
				return true;
			}
			return false;
		}

		inline bool
			filter_left_lines(const std::tuple<cv::Rect, std::wstring, cv::Range>& basis, const std::vector<block>& blocks, const float hitting_threshold = 0.5, const float expanded_ratio = 0.0, const float cutting_ratio = 100.0, bool expand_both_sides = false)
		{
			std::vector<std::pair<cv::Rect, std::wstring>> found_lines;

			const auto horizontal_lines = find_horizontal_lines(basis, blocks, hitting_threshold, expanded_ratio, expand_both_sides);

			for (const auto& line : horizontal_lines) {
				const auto line_rect = std::get<0>(line);
				if (std::get<0>(basis).x >= line_rect.br().x)
					found_lines.emplace_back(line);
			}

			boost::remove_erase_if(found_lines, [&basis, &cutting_ratio](const std::pair<cv::Rect, std::wstring>& block) {
				const auto cut = to_rect(basis).x - to_rect(basis).width * cutting_ratio;
				return to_rect(block).br().x < cut;
			});

			std::sort(std::begin(found_lines), std::end(found_lines),
				[](const std::pair<cv::Rect, std::wstring>& a, const std::pair<cv::Rect, std::wstring>& b) {
				return std::get<0>(a).x > std::get<0>(b).x;
			});

			return found_lines.size() > 0 ? true : false;
		}


		inline std::vector<std::pair<cv::Rect, std::wstring>>
			find_nearest_right_line(const std::tuple<cv::Rect, std::wstring, cv::Range>& basis, const std::vector<block>& blocks, const float hitting_threshold = 0.5, const float expanded_ratio = 0.0, const float cutting_ratio = 100.0, bool expand_both_sides = false)
		{
			const auto found_lines = find_right_lines(basis, blocks, hitting_threshold, expanded_ratio, cutting_ratio, expand_both_sides);

			if (found_lines.empty())
				return std::vector<std::pair<cv::Rect, std::wstring>>();

			return std::vector<std::pair<cv::Rect, std::wstring>>{found_lines[0]};
		}

		inline std::vector<std::pair<cv::Rect, std::wstring>>
			find_left_lines(const std::tuple<cv::Rect, std::wstring, cv::Range>& basis, const std::vector<block>& blocks, const float hitting_threshold = 0.5, const float expanded_ratio = 0.0, const float cutting_ratio = 100.0, bool expand_both_sides = false)
		{
			std::vector<std::pair<cv::Rect, std::wstring>> found_lines;

			const auto horizontal_lines = find_horizontal_lines(basis, blocks, hitting_threshold, expanded_ratio, expand_both_sides);

			for (const auto& line : horizontal_lines) {
				const auto line_rect = std::get<0>(line);
				if (std::get<0>(basis).x >= line_rect.br().x)
					found_lines.emplace_back(line);
			}

			boost::remove_erase_if(found_lines, [&basis, &cutting_ratio](const std::pair<cv::Rect, std::wstring>& block) {
				const auto cut = to_rect(basis).x - to_rect(basis).width * cutting_ratio;
				return to_rect(block).br().x < cut;
			});

			std::sort(std::begin(found_lines), std::end(found_lines),
				[](const std::pair<cv::Rect, std::wstring>& a, const std::pair<cv::Rect, std::wstring>& b) {
				return std::get<0>(a).x > std::get<0>(b).x;
			});

			return found_lines;
		}


		inline std::vector<std::pair<cv::Rect, std::wstring>>
			find_far_left_line(const std::tuple<cv::Rect, std::wstring, cv::Range>& basis, const std::vector<block>& blocks, const float hitting_threshold = 0.5, const float expanded_ratio = 0.0, const float cutting_ratio = 100.0, bool expand_both_sides = false)
		{
			auto found_lines = find_left_lines(basis, blocks, hitting_threshold, expanded_ratio, cutting_ratio, expand_both_sides);

			std::sort(std::begin(found_lines), std::end(found_lines),
				[](const std::pair<cv::Rect, std::wstring>& a, const std::pair<cv::Rect, std::wstring>& b) {
				return std::get<0>(a).x < std::get<0>(b).x;
			});

			if (found_lines.empty())
				return std::vector<std::pair<cv::Rect, std::wstring>>();

			return std::vector<std::pair<cv::Rect, std::wstring>>{found_lines[0]};
		}


		inline std::vector<std::pair<cv::Rect, std::wstring>>
			find_nearest_left_line(const std::tuple<cv::Rect, std::wstring, cv::Range>& basis, const std::vector<block>& blocks, const float hitting_threshold = 0.5, const float expanded_ratio = 0.0, const float cutting_ratio = 100.0, bool expand_both_sides = false)
		{
			const auto found_lines = find_left_lines(basis, blocks, hitting_threshold, expanded_ratio, cutting_ratio, expand_both_sides);

			if (found_lines.empty())
				return std::vector<std::pair<cv::Rect, std::wstring>>();

			return std::vector<std::pair<cv::Rect, std::wstring>>{found_lines[0]};
		}

		inline std::vector<std::pair<cv::Rect, std::wstring>>
			find_up_words(const std::tuple<cv::Rect, std::wstring, cv::Range>& basis, const std::vector<block>& blocks, const float hitting_threshold = 0.5, const float expanded_ratio = 0.0, const float cutting_ratio = 100.0, bool expand_both_sides = false)
		{
			std::vector<std::pair<cv::Rect, std::wstring>> found_lines;

			const auto vertical_lines = find_vertical_words(basis, blocks, hitting_threshold, expanded_ratio, expand_both_sides);
			const auto basis_rect = to_rect(basis);

			for (const auto& line : vertical_lines) {
				const auto line_rect = std::get<0>(line);
				const auto basis_center_y = basis_rect.y + basis_rect.height / 2;
				if (basis_center_y > line_rect.y)
					found_lines.emplace_back(line);
			}

			boost::remove_erase_if(found_lines, [&basis, &cutting_ratio](const std::pair<cv::Rect, std::wstring>& block) {
				const auto cut = to_rect(basis).y - to_rect(basis).height * cutting_ratio;
				return to_rect(block).y < cut;
			});

			std::sort(std::begin(found_lines), std::end(found_lines),
				[](const std::pair<cv::Rect, std::wstring>& a, const std::pair<cv::Rect, std::wstring>& b) {
				return std::get<0>(a).y > std::get<0>(b).y;
			});

			return found_lines;
		}

		inline std::vector<std::pair<cv::Rect, std::wstring>>
			find_up_lines(const std::tuple<cv::Rect, std::wstring, cv::Range>& basis, const std::vector<block>& blocks, const float hitting_threshold = 0.5, const float expanded_ratio = 0.0, const float cutting_ratio = 100.0, bool expand_both_sides = false)
		{
			std::vector<std::pair<cv::Rect, std::wstring>> found_lines;

			const auto vertical_lines = find_vertical_lines(basis, blocks, hitting_threshold, expanded_ratio, expand_both_sides);
			const auto basis_rect = to_rect(basis);

			for (const auto& line : vertical_lines) {
				const auto line_rect = std::get<0>(line);
				const auto basis_center_y = basis_rect.y + basis_rect.height / 2;
				if (basis_center_y > line_rect.y)
					found_lines.emplace_back(line);
			}

			boost::remove_erase_if(found_lines, [&basis, &cutting_ratio](const std::pair<cv::Rect, std::wstring>& block) {
				const auto cut = to_rect(basis).y - to_rect(basis).height * cutting_ratio;
				return to_rect(block).y < cut;
			});

			std::sort(std::begin(found_lines), std::end(found_lines),
				[](const std::pair<cv::Rect, std::wstring>& a, const std::pair<cv::Rect, std::wstring>& b) {
				return std::get<0>(a).y > std::get<0>(b).y;
			});

			return found_lines;
		}

		inline std::vector<std::pair<cv::Rect, std::wstring>>
			find_nearest_up_words(const std::tuple<cv::Rect, std::wstring, cv::Range>& basis, const std::vector<block>& blocks, const float hitting_threshold = 0.5, const float expanded_ratio = 0.0, const float cutting_ratio = 100.0, bool multi_line = false, bool merge = false, bool expand_both_sides = false)
		{
			auto found_lines = find_up_words(basis, blocks, hitting_threshold, expanded_ratio, cutting_ratio, expand_both_sides);

			if (found_lines.empty())
				return std::vector<std::pair<cv::Rect, std::wstring>>();

			if (!multi_line)
				return std::vector<std::pair<cv::Rect, std::wstring>>{found_lines[0]};

			std::vector<std::pair<cv::Rect, std::wstring>> found_above_lines;
			for (int i = 0; i < found_lines.size(); i++) {
				if (i == 0) {
					if (to_wstring(found_lines[i]).size() > 1)
						found_above_lines.emplace_back(found_lines[i]);
					else {
						found_lines.erase(found_lines.begin());
						i--;
					}
					continue;
				}

				auto line_rect = to_rect(found_lines[i]);
				auto prev_line_rect = to_rect(found_lines[i + 1]);

				if (prev_line_rect.y - prev_line_rect.height * 1.5 > line_rect.y) {
					break;
				}
				else {
					found_above_lines.emplace_back(found_lines[i]);
				}
			}

			if (merge) {
				return std::vector<std::pair<cv::Rect, std::wstring>>{std::make_pair(to_rect(found_above_lines), to_wstring(found_above_lines))};
			}

			return found_above_lines;
		}

		inline std::vector<std::pair<cv::Rect, std::wstring>>
			find_nearest_up_lines(const std::tuple<cv::Rect, std::wstring, cv::Range>& basis, const std::vector<block>& blocks, const float hitting_threshold = 0.5, const float expanded_ratio = 0.0, const float cutting_ratio = 100.0, bool multi_line = false, bool merge = false, bool expand_both_sides = false)
		{
			auto found_lines = find_up_lines(basis, blocks, hitting_threshold, expanded_ratio, cutting_ratio, expand_both_sides);

			if (found_lines.empty())
				return std::vector<std::pair<cv::Rect, std::wstring>>();

			if (!multi_line)
				return std::vector<std::pair<cv::Rect, std::wstring>>{found_lines[0]};

			std::vector<std::pair<cv::Rect, std::wstring>> found_above_lines;
			for (int i = 0; i < found_lines.size(); i++) {
				if (i == 0) {
					if (to_wstring(found_lines[i]).size() > 1)
						found_above_lines.emplace_back(found_lines[i]);
					else {
						found_lines.erase(found_lines.begin());
						i--;
					}
					continue;
				}

				auto line_rect = to_rect(found_lines[i]);
				auto prev_line_rect = to_rect(found_lines[i + 1]);

				if (prev_line_rect.y - prev_line_rect.height * 1.5 > line_rect.y) {
					break;
				}
				else {
					found_above_lines.emplace_back(found_lines[i]);
				}
			}

			if (merge) {
				return std::vector<std::pair<cv::Rect, std::wstring>>{std::make_pair(to_rect(found_above_lines), to_wstring(found_above_lines))};
			}

			return found_above_lines;
		}

		inline std::vector<std::pair<cv::Rect, std::wstring>>
			find_down_lines(const std::tuple < cv::Rect, std::wstring, cv::Range>& basis, const std::vector<block>& blocks, const float hitting_threshold = 0.5, const float expanded_ratio = 0.0, const float cutting_ratio = 100.0, bool expand_both_sides = false)
		{
			std::vector<std::pair<cv::Rect, std::wstring>> found_lines;

			const auto vertical_lines = find_vertical_lines(basis, blocks, hitting_threshold, expanded_ratio, expand_both_sides);
			const auto basis_rect = to_rect(basis);

			for (const auto& line : vertical_lines) {
				const auto line_rect = std::get<0>(line);
				const auto basis_center_y = basis_rect.y + basis_rect.height / 2;
				if (basis_center_y < line_rect.y)
					found_lines.emplace_back(line);
			}

			boost::remove_erase_if(found_lines, [&basis, &cutting_ratio](const std::pair<cv::Rect, std::wstring>& block) {
				const auto cut = to_rect(basis).br().y + to_rect(basis).height * cutting_ratio;
				return to_rect(block).y > cut;
			});

			std::sort(std::begin(found_lines), std::end(found_lines),
				[](const std::pair<cv::Rect, std::wstring>& a, const std::pair<cv::Rect, std::wstring>& b) {
				return std::get<0>(a).y < std::get<0>(b).y;
			});

			return found_lines;
		}

		inline std::vector<std::pair<cv::Rect, std::wstring>>
			find_down_characters(const std::tuple < cv::Rect, std::wstring, cv::Range>& basis, const std::vector<block>& blocks, const float hitting_threshold = 0.5, const float expanded_ratio = 0.0, const float cutting_ratio = 100.0, bool expand_both_sides = false, const bool restrict_right = false, const float restrict_ratio = 0)
		{
			std::vector<std::pair<cv::Rect, std::wstring>> found_lines;

			const auto vertical_lines = find_vertical_characters(basis, blocks, hitting_threshold, expanded_ratio, expand_both_sides, restrict_right, restrict_ratio);
			const auto basis_rect = to_rect(basis);

			for (const auto& line : vertical_lines) {
				const auto line_rect = std::get<0>(line);
				const auto basis_center_y = basis_rect.y + basis_rect.height / 2;
				if (basis_center_y < line_rect.y)
					found_lines.emplace_back(line);
			}

			boost::remove_erase_if(found_lines, [&basis, &cutting_ratio](const std::pair<cv::Rect, std::wstring>& block) {
				const auto cut = to_rect(basis).br().y + to_rect(basis).height * cutting_ratio;
				return to_rect(block).y > cut;
			});

			std::sort(std::begin(found_lines), std::end(found_lines),
				[](const std::pair<cv::Rect, std::wstring>& a, const std::pair<cv::Rect, std::wstring>& b) {
				return std::get<0>(a).y < std::get<0>(b).y;
			});

			return found_lines;
		}


		inline std::vector<std::pair<cv::Rect, std::wstring>>
			find_down_words(const std::tuple < cv::Rect, std::wstring, cv::Range>& basis, const std::vector<block>& blocks, const float hitting_threshold = 0.5, const float expanded_ratio = 0.0, const float cutting_ratio = 100.0, bool expand_both_sides = false, const bool restrict_right = false, const float restrict_ratio = 0)
		{
			std::vector<std::pair<cv::Rect, std::wstring>> found_lines;

			const auto vertical_lines = find_vertical_words(basis, blocks, hitting_threshold, expanded_ratio, expand_both_sides, restrict_right, restrict_ratio);
			const auto basis_rect = to_rect(basis);

			for (const auto& line : vertical_lines) {
				const auto line_rect = std::get<0>(line);
				const auto basis_center_y = basis_rect.y + basis_rect.height / 2;
				if (basis_center_y < line_rect.y)
					found_lines.emplace_back(line);
			}

			boost::remove_erase_if(found_lines, [&basis, &cutting_ratio](const std::pair<cv::Rect, std::wstring>& block) {
				const auto cut = to_rect(basis).br().y + to_rect(basis).height * cutting_ratio;
				return to_rect(block).y > cut;
			});

			std::sort(std::begin(found_lines), std::end(found_lines),
				[](const std::pair<cv::Rect, std::wstring>& a, const std::pair<cv::Rect, std::wstring>& b) {
				return std::get<0>(a).y < std::get<0>(b).y;
			});

			return found_lines;
		}


		inline std::vector<std::pair<cv::Rect, std::wstring>>
			find_coa_nearest_down_lines(const std::tuple<cv::Rect, std::wstring, cv::Range>& basis, const std::vector<block>& blocks, const float hitting_threshold = 0.5, const float expanded_ratio = 0.0, const float cutting_ratio = 100.0, bool multi_line = false, bool merge = false, bool expand_both_sides = false, const float next_line_ratio = 1.5)
		{
			auto found_lines = find_down_lines(basis, blocks, hitting_threshold, expanded_ratio, cutting_ratio, expand_both_sides);

			if (found_lines.empty())
				return std::vector<std::pair<cv::Rect, std::wstring>>();

			if (!multi_line)
				return std::vector<std::pair<cv::Rect, std::wstring>>{found_lines[0]};

			std::vector<std::pair<cv::Rect, std::wstring>> found_below_lines;
			for (int i = 0; i < found_lines.size(); i++) {
				if (i == 0) {
					if (to_wstring(found_lines[i]).size() > 1 || to_wstring(found_lines[i]).compare(L"%") == 0)
						found_below_lines.emplace_back(found_lines[i]);
					else {
						found_lines.erase(found_lines.begin());
						i--;
					}
					continue;
				}
				auto line_rect = to_rect(found_lines[i]);
				auto prev_line_rect = to_rect(found_lines[i - 1]);
				if (prev_line_rect.br().y + prev_line_rect.height * next_line_ratio < line_rect.y) {
					break;
				}
				else {
					found_below_lines.emplace_back(found_lines[i]);
				}
			}
			if (merge) {
				return std::vector<std::pair<cv::Rect, std::wstring>>{std::make_pair(to_rect(found_below_lines), to_wstring(found_below_lines))};
			}

			return found_below_lines;
		}

		inline std::vector<std::pair<cv::Rect, std::wstring>>
			find_coa_nearest_down_words2(const std::tuple<cv::Rect, std::wstring, cv::Range>& basis, const std::vector<block>& blocks, const float hitting_threshold = 0.5, const float expanded_ratio = 0.0, const float cutting_ratio = 100.0, bool multi_line = false, bool merge = false, bool expand_both_sides = false, const float next_line_ratio = 1.5, const bool restrict_right = false, const float restrict_ratio = 0)
		{
			auto found_lines = find_down_words(basis, blocks, hitting_threshold, expanded_ratio, cutting_ratio, expand_both_sides, restrict_right, restrict_ratio);

			if (found_lines.empty())
				return std::vector<std::pair<cv::Rect, std::wstring>>();

			if (!multi_line)
				return std::vector<std::pair<cv::Rect, std::wstring>>{found_lines[0]};

			std::vector<std::pair<cv::Rect, std::wstring>> found_below_lines;
			for (int i = 0; i < found_lines.size(); i++) {
				if (i == 0) {
					if (to_wstring(found_lines[i]).size() > 1 || to_wstring(found_lines[i]).compare(L"%") == 0)
						found_below_lines.emplace_back(found_lines[i]);
					else {
						found_lines.erase(found_lines.begin());
						i--;
					}
					continue;
				}
				auto line_rect = to_rect(found_lines[i]);
				auto prev_line_rect = to_rect(found_lines[i - 1]);
				int prev_prev_line_height = i >= 2 ? to_rect(found_lines[i - 2]).height : 0;
				if (prev_line_rect.br().y + std::max(prev_line_rect.height, prev_prev_line_height) * next_line_ratio < line_rect.y) {
					break;
				}
				else {
					found_below_lines.emplace_back(found_lines[i]);
				}
			}
			if (merge) {
				return std::vector<std::pair<cv::Rect, std::wstring>>{std::make_pair(to_rect(found_below_lines), to_wstring(found_below_lines))};
			}

			return found_below_lines;
		}

		inline std::vector<std::pair<cv::Rect, std::wstring>>
			find_coa_nearest_down_words(const std::tuple<cv::Rect, std::wstring, cv::Range>& basis, const std::vector<block>& blocks, const float hitting_threshold = 0.5, const float expanded_ratio = 0.0, const float cutting_ratio = 100.0, bool multi_line = false, bool merge = false, bool expand_both_sides = false, const float next_line_ratio = 1.5, const bool restrict_right = false, const float restrict_ratio = 0)
		{
			auto found_lines = find_down_words(basis, blocks, hitting_threshold, expanded_ratio, cutting_ratio, expand_both_sides, restrict_right, restrict_ratio);

			if (found_lines.empty())
				return std::vector<std::pair<cv::Rect, std::wstring>>();

			if (!multi_line)
				return std::vector<std::pair<cv::Rect, std::wstring>>{found_lines[0]};

			std::vector<std::pair<cv::Rect, std::wstring>> found_below_lines;
			for (int i = 0; i < found_lines.size(); i++) {
				if (i == 0) {
					if (to_wstring(found_lines[i]).size() > 1 || to_wstring(found_lines[i]).compare(L"%") == 0)
						found_below_lines.emplace_back(found_lines[i]);
					else {
						found_lines.erase(found_lines.begin());
						i--;
					}
					continue;
				}
				auto line_rect = to_rect(found_lines[i]);
				auto prev_line_rect = to_rect(found_lines[i - 1]);
				if (prev_line_rect.br().y + prev_line_rect.height * next_line_ratio < line_rect.y) {
					break;
				}
				else {
					found_below_lines.emplace_back(found_lines[i]);
				}
			}
			if (merge) {
				return std::vector<std::pair<cv::Rect, std::wstring>>{std::make_pair(to_rect(found_below_lines), to_wstring(found_below_lines))};
			}

			return found_below_lines;
		}



		inline std::vector<std::pair<cv::Rect, std::wstring>>
			find_nearest_down_lines(const std::tuple<cv::Rect, std::wstring, cv::Range>& basis, const std::vector<block>& blocks, const float hitting_threshold = 0.5, const float expanded_ratio = 0.0, const float cutting_ratio = 100.0, bool multi_line = false, bool merge = false, bool expand_both_sides = false)
		{
			auto found_lines = find_down_lines(basis, blocks, hitting_threshold, expanded_ratio, cutting_ratio, expand_both_sides);

			if (found_lines.empty())
				return std::vector<std::pair<cv::Rect, std::wstring>>();

			if (!multi_line)
				return std::vector<std::pair<cv::Rect, std::wstring>>{found_lines[0]};

			std::vector<std::pair<cv::Rect, std::wstring>> found_below_lines;


			for (int i = 0; i < found_lines.size(); i++) {
				if (i == 0) {
					if (to_wstring(found_lines[i]).size() > 1)
						found_below_lines.emplace_back(found_lines[i]);
					else {
						found_lines.erase(found_lines.begin());
						i--;
					}
					continue;
				}
				auto line_rect = to_rect(found_lines[i]);
				auto prev_line_rect = to_rect(found_lines[i - 1]);
				if (prev_line_rect.br().y + prev_line_rect.height * 1.5 < line_rect.y) {
					break;
				}
				else {
					found_below_lines.emplace_back(found_lines[i]);
				}
			}
			if (merge) {
				return std::vector<std::pair<cv::Rect, std::wstring>>{std::make_pair(to_rect(found_below_lines), to_wstring(found_below_lines))};
			}

			return found_below_lines;
		}

		inline std::vector<std::pair<cv::Rect, std::wstring>>
			find_same_blocks(const std::pair<cv::Rect, std::wstring>& basis, const std::vector<block>& blocks)
		{
			std::vector<std::pair<cv::Rect, std::wstring>> found_blocks;
			const auto basis_rect = cv::Rect(std::get<0>(basis));

			for (const auto& block : blocks) {
				for (const auto& line : block) {
					if (to_rect(line) == basis_rect) {
						std::transform(std::begin(block), std::end(block), std::back_inserter(found_blocks), [](const ocr::line& line) {
							return std::make_pair(to_rect(line), to_wstring(line));
						});

						return found_blocks;
					}
				}
			}

			return found_blocks;
		}

		inline std::vector<std::pair<cv::Rect, std::wstring>>
			search_self(const std::tuple<cv::Rect, std::wstring, cv::Range>& basis, const std::vector<block>& blocks)
		{
			return std::vector<std::pair<cv::Rect, std::wstring>>{
				std::make_pair(std::get<0>(basis),
					std::wstring(std::get<1>(basis).begin() + std::get<2>(basis).end, std::get<1>(basis).end()))
			};
		}

		inline std::vector<std::pair<cv::Rect, std::wstring>>
			search_self_all(const std::tuple<cv::Rect, std::wstring, cv::Range>& basis, const std::vector<block>& blocks)
		{
			return std::vector<std::pair<cv::Rect, std::wstring>>{
				std::make_pair(std::get<0>(basis), std::get<1>(basis))
			};
		}

		static std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>> line_field_to_word_field(const std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>& field,
			const std::vector<block>& blocks) {
			std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>> ret;
			for (const auto& line_field : field) {
				for (const auto& block : blocks) {
					for (const auto& line : block) {
						if (to_rect(line) == std::get<0>(line_field)) {
							int char_size = 0;
							for (const auto& word : line) {
								char_size += word.size();
							}
							char_size += (line.size() - 1);
							std::wstring line_str = std::get<1>(line_field);
							int line_length = line_str.length();
							float ratio = char_size / (float)line_length;
							cv::Range range = std::get<2>(line_field);
							int field_start = range.start * ratio;
							int field_end = range.end * ratio;

							std::vector<bool> temp_counts(line.size(), false);

							int start_index = 0;
							for (int i = 0; i < line.size(); ++i) {
								const auto& word = line[i];
								int end_index = start_index + word.size();
								int gap = std::min(field_end, end_index) - std::max(field_start, start_index);
								if (gap > word.size() * 0.8) {
									temp_counts[i] = true;
								}
								start_index = end_index + 1;
							}
							cv::Rect new_rect = cv::Rect();
							for (int i = 0; i < line.size(); ++i) {
								if (temp_counts[i]) {
									new_rect = new_rect | to_rect(line[i]);
								}
							}

							if (new_rect.width == 0) {
								int start_index = 0;
								std::vector<float> temp_ratios(line.size(), 0.0f);
								for (int i = 0; i < line.size(); ++i) {
									const auto& word = line[i];
									int end_index = start_index + word.size();
									int gap = std::min(field_end, end_index) - std::max(field_start, start_index);
									temp_ratios[i] = gap / (float)word.size();
									start_index = end_index + 1;
								}
								int index = distance(temp_ratios.begin(), std::max_element(temp_ratios.begin(), temp_ratios.end()));
								new_rect = to_rect(line[index]);
							}

							std::wstring new_str = line_str.substr(range.start, range.end - range.start);
							ret.push_back(std::make_tuple(new_rect, new_str, cv::Range(0, new_str.length())));
						}
					}
				}
			}
			return ret;
		}

		inline std::vector<std::pair<cv::Rect, std::wstring>>
			search_line(const std::tuple<cv::Rect, std::wstring, cv::Range>& basis, const std::vector<block>& blocks)
		{
			return std::vector<std::pair<cv::Rect, std::wstring>>{
				std::make_pair(std::get<0>(basis), std::get<1>(basis)),
			};
		}

		inline std::vector<std::pair<cv::Rect, std::wstring>>
			default_search(const std::tuple<cv::Rect, std::wstring, cv::Range>& basis, const std::vector<block>& blocks)
		{
			std::vector< std::pair < cv::Rect, std::wstring >> found_blocks;

			for (const auto& block : blocks) {
				std::transform(std::begin(block), std::end(block), std::back_inserter(found_blocks), [](const line& line) {
					return std::make_pair(to_rect(line), to_wstring(line));
				});
			}

			return found_blocks;
		}

		inline std::pair<cv::Rect, std::wstring>
			default_preprocess(const std::pair<cv::Rect, std::wstring>& a)
		{
			return a;
		}

		inline std::wstring
			default_extract(const std::pair<cv::Rect, std::wstring>& a)
		{
			return std::get<1>(a);
		}

		inline std::wstring
			postprocess_only_date(const std::wstring& a) {
			return boost::regex_replace(a, boost::wregex(L"[^0-9./]"), L"");
		}

		inline std::wstring
			default_postprocess(const std::wstring& a)
		{
			return boost::algorithm::trim_copy(a);
		}

		inline std::wstring
			postprocess_uppercase(const std::wstring& a)
		{
			return boost::algorithm::trim_copy(boost::to_upper_copy(a));
		}
		
		inline std::pair<cv::Rect, std::wstring>
			preprocess_port_of_discharge(const std::pair<cv::Rect, std::wstring>& a)
		{
			auto text = std::get<1>(a);
			text = boost::replace_all_copy(text, L"»*", L"**");
			text = boost::replace_all_copy(text, L"*»", L"**");
			//text = boost::regex_replace(text, boost::wregex(L"»*"), L"**");
			//text = boost::regex_replace(text, boost::wregex(L"*»"), L"**");
			//text = boost::regex_replace(text, boost::wregex(L"/?FLIGHT to:?", boost::regex::icase), L"");

			return std::make_pair(std::get<0>(a), text);
		}
		
		inline std::pair<cv::Rect, std::wstring>
			preprocess_bank_name(const std::pair<cv::Rect, std::wstring>& a)
		{
			auto text = std::get<1>(a);
			text = boost::replace_all_copy(text, L":", L"");
			text = boost::replace_all_copy(text, L"RES애A BANK", L"RESONA BANK");

			return std::make_pair(std::get<0>(a), text);
		}

		inline std::pair<cv::Rect, std::wstring>
			preprocess_sender(const std::pair<cv::Rect, std::wstring>& a)
		{
			auto text = std::get<1>(a);
			text = boost::regex_replace(text, boost::wregex(L"[^a-zA-Z0-9\\., ]"), L"");
			//text = boost::replace_all_copy(text, L":", L"");
			//text = boost::replace_all_copy(text, L"RES애A BANK", L"RESONA BANK");

			return std::make_pair(std::get<0>(a), text);
		}


		inline std::pair<cv::Rect, std::wstring>
			preprocess_amount(const std::pair<cv::Rect, std::wstring>& a)
		{
			auto text = std::get<1>(a);
			text = boost::replace_all_copy(text, L":", L"");
			text = boost::replace_all_copy(text, L"1LC", L"ILC");
			text = boost::replace_all_copy(text, L"0SD", L"USD");

			return std::make_pair(std::get<0>(a), text);
		}

		inline std::pair<cv::Rect, std::wstring>
			preprocess_port_of_loading(const std::pair<cv::Rect, std::wstring>& a)
		{
			auto text = std::get<1>(a);
			text = boost::replace_all_copy(text, L"♦*", L"");
			
			//text = boost::regex_replace(text, boost::wregex(L"»*"), L"**");
			//text = boost::regex_replace(text, boost::wregex(L"*»"), L"**");
			//text = boost::regex_replace(text, boost::wregex(L"/?FLIGHT to:?", boost::regex::icase), L"");

			return std::make_pair(std::get<0>(a), text);
		}

		inline std::wstring
			postprocess_discharge(const std::wstring& a)
		{
			//std::wstring requester = boost::regex_replace(std::get<1>(request_organ), boost::wregex(L" "), L"");
			//requester = boost::regex_replace(requester, boost::wregex(L" "), L"");
			//std::make_pair(std::get<0>(email), email_address);
			return boost::algorithm::trim_copy(boost::to_upper_copy(a));
		}


		inline std::vector<std::pair<cv::Rect, std::wstring>>
			extract_field_values(const std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>& fields, const std::vector<block>& blocks,
			const std::function < std::vector<std::pair<cv::Rect, std::wstring>>(
			const std::tuple<cv::Rect, std::wstring, cv::Range>&, const std::vector<block>&)>& search,
			const std::function<std::pair<cv::Rect, std::wstring>(const std::pair<cv::Rect, std::wstring>&)>& preprocess,
			const std::function<std::wstring(const std::pair<cv::Rect, std::wstring>&)>& extract,
			const std::function<std::wstring(const std::wstring&)>& postprocess)
		{
			std::vector<std::pair<cv::Rect, std::wstring>> extracted_values;

			for (const auto& field : fields) {
				const auto found_blocks = search(field, blocks);

				for (const auto& line : found_blocks) {
					const auto preprocessed = preprocess(line);
					const auto extracted = extract(preprocessed);
					const auto postprocessed = postprocess(extracted);

					if (!postprocessed.empty()) {
						extracted_values.emplace_back(std::make_pair(std::get<0>(line), postprocessed));
					}
				}
			}

			return extracted_values;
		}

		inline std::vector<std::pair<cv::Rect, std::wstring>>
			extract_field_values2(const std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>& fields, const std::vector<block>& blocks,
				const std::function < std::vector<std::pair<cv::Rect, std::wstring>>(
					const std::tuple<cv::Rect, std::wstring, cv::Range>&, const std::vector<block>&)>& search,
				const std::function<std::pair<cv::Rect, std::wstring>(const std::pair<cv::Rect, std::wstring>&)>& preprocess,
				const std::function<std::wstring(const std::pair<cv::Rect, std::wstring>&)>& extract,
				const std::function<std::wstring(const std::wstring&)>& postprocess)
		{
			std::vector<std::pair<cv::Rect, std::wstring>> extracted_values;

			int j = 0;
			for (const auto& field : fields) {
				if (j != fields.size() - 1) {
					j++;
					continue;
				}
				const auto found_blocks = search(field, blocks);

				if (found_blocks.size() == 0)
					continue;

				std::pair<cv::Rect, std::wstring> total_line = found_blocks[0];
				std::wstring whole_text = L"";
				for (int i = 0; i < found_blocks.size(); i++) {
					const auto line = found_blocks[i];
					whole_text += line.second;
					whole_text += L" ";
				}

				total_line.second = whole_text;
				const auto preprocessed = preprocess(total_line);
				const auto extracted = extract(preprocessed);
				const auto postprocessed = postprocess(extracted);

				if (!postprocessed.empty()) {
					extracted_values.emplace_back(std::make_pair(std::get<0>(total_line), postprocessed));
				}

			}

			return extracted_values;
		}

		inline std::vector<std::pair<cv::Rect, std::wstring>>
			extract_field_values_2(const std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>& fields, const std::vector<block>& blocks,
			const std::vector<std::function <bool(const std::tuple<cv::Rect, std::wstring, cv::Range>&, const std::vector<block>&)>>& filter_fields,
			const std::function < std::vector<std::pair<cv::Rect, std::wstring>>(
			const std::tuple<cv::Rect, std::wstring, cv::Range>&, const std::vector<block>&)>& search,
			const std::function<std::pair<cv::Rect, std::wstring>(const std::pair<cv::Rect, std::wstring>&)>& preprocess,
			const std::function<std::wstring(const std::pair<cv::Rect, std::wstring>&)>& extract,
			const std::function<std::wstring(const std::wstring&)>& postprocess)
		{
			std::vector<std::pair<cv::Rect, std::wstring>> extracted_values;

			for (const auto& field : fields) {
				bool flag = false;
				for (const auto filter_field : filter_fields) {
					if (filter_field(field, blocks)) {
						flag = true;
					}
				}
				if (flag) {
					continue;
				}

				const auto found_blocks = search(field, blocks);

				for (const auto& line : found_blocks) {
					const auto preprocessed = preprocess(line);
					const auto extracted = extract(preprocessed);
					const auto postprocessed = postprocess(extracted);

					if (!postprocessed.empty()) {
						extracted_values.emplace_back(std::make_pair(std::get<0>(line), postprocessed));
					}
				}
			}

			return extracted_values;
		}

		inline std::function<std::wstring(const std::wstring&)>
			create_postprocess_function(const configuration& configuration, const std::wstring& mapper)
		{
			return[&](const std::wstring& text) -> std::wstring {
				std::unordered_map<std::wstring, std::wstring> processed_maps;
				std::wifstream map_file(fmt::format(L"{}\\{}", get_profile_directory(configuration), mapper));
				text::csv::csv_wistream csvs(map_file);
				const auto lower_text = boost::to_lower_copy(text);

				std::wstring country;
				std::wstring country_code;

				while (csvs) {
					csvs >> country >> country_code;
					processed_maps.emplace(std::make_pair(boost::to_lower_copy(country), country_code));
				}

				if (processed_maps.find(lower_text) != processed_maps.end())
					return processed_maps.at(lower_text);
				return boost::to_upper_copy(text);
			};
		}

		inline std::function<std::wstring(const std::pair<cv::Rect, std::wstring>&)>
			create_extract_ori_function(const configuration& configuration, const std::wstring& dictionary_category,
			int edit_distance = 1)
		{
			return [&, edit_distance](const std::pair<cv::Rect, std::wstring>& line) -> std::wstring {
				const auto dictionary = get_dictionary_words(configuration, dictionaries_, dictionary_category);

				aho_corasick::wtrie trie;
				trie.case_insensitive().remove_overlaps().allow_space();
				build_trie(trie, dictionary);
				const auto spell_dictionary = build_spell_dictionary(configuration, dictionary_category);

				auto text = boost::to_lower_copy(std::wstring(std::get<1>(line)));
				std::vector<std::wstring> words;
				boost::algorithm::split(words, text, boost::is_any_of(L"\t "), boost::token_compress_on);

				for (auto& word : words) {
					const auto suggested = spell_dictionary->Correct(word);
					if (!suggested.empty() && suggested[0].distance <= edit_distance)
						word = suggested[0].term;
				}

				text = boost::algorithm::join(words, L" ");

				auto suggested = spell_dictionary->Correct(text);
				if (!suggested.empty() && suggested[0].distance <= edit_distance)
					text = suggested[0].term;

				auto matches = trie.parse_text(text);

				if (matches.empty()) {
					boost::erase_all(text, L" ");
					const auto suggested = spell_dictionary->Correct(text);
					if (!suggested.empty() && suggested[0].distance <= edit_distance)
						text = suggested[0].term;
					matches = trie.parse_text(text);
				}

				if (matches.empty())
					return L"";

				return std::get<1>(line);
			};
		}

		inline std::function<std::wstring(const std::pair<cv::Rect, std::wstring>&)>
			create_extract_function(const configuration& configuration, const std::wstring& dictionary_category,
			int edit_distance = 1)
		{
			return [&, edit_distance](const std::pair<cv::Rect, std::wstring>& line) -> std::wstring {
				const auto dictionary = get_dictionary_words(configuration, dictionaries_, dictionary_category);

				aho_corasick::wtrie trie;
				trie.case_insensitive().remove_overlaps().allow_space();
				build_trie(trie, dictionary);
				const auto spell_dictionary = build_spell_dictionary(configuration, dictionary_category);

				auto text = boost::to_lower_copy(std::wstring(std::get<1>(line)));
				std::vector<std::wstring> words;
				boost::algorithm::split(words, text, boost::is_any_of(L"\t "), boost::token_compress_on);

				for (auto& word : words) {
					const auto suggested = spell_dictionary->Correct(word);
					if (!suggested.empty() && suggested[0].distance <= edit_distance)
						word = suggested[0].term;
				}

				text = boost::algorithm::join(words, L" ");

				auto suggested = spell_dictionary->Correct(text);
				if (!suggested.empty() && suggested[0].distance <= edit_distance)
					text = suggested[0].term;

				auto matches = trie.parse_text(text);

				if (matches.empty()) {
					boost::erase_all(text, L" ");
					const auto suggested = spell_dictionary->Correct(text);
					if (!suggested.empty() && suggested[0].distance <= edit_distance)
						text = suggested[0].term;
					matches = trie.parse_text(text);
				}

				if (matches.empty())
					return L"";

				if (dictionary_category == L"requester") {
					if ((text.size() > 10 && spell_dictionary->Correct(matches.back().get_keyword())[0].term.size() <= 3)
						|| to_rect(line).height < 18)
						return L"";

					// TODO 2가지 경우 고려해야함
					// #01. 서울세관 서울특별시 => 서울세관 나오게 변경 (20181004_set_13 (1))
					// #02. 서울지방국세청 양천세무서 => 양천 세무서 나오게 변경 (20181004_set_1 (2), 20181004_set_24 (1))
					if (matches.size() > 1) {
						auto suspected_requester = spell_dictionary->Correct(matches.back().get_keyword())[0].term;
						if (suspected_requester.size() >= 3 &&
							suspected_requester.find_last_of(L"시") != std::wstring::npos
							&& suspected_requester.find_last_of(L"시") == suspected_requester.size() - 1) {
							return spell_dictionary->Correct(matches.front().get_keyword())[0].term;
						}

						return suspected_requester;
					}
				}
				else if (dictionary_category == L"position") {
					const auto suggested = spell_dictionary->Correct(matches.back().get_keyword());
					return suggested.empty() ? matches.back().get_keyword() : suggested[0].term;
				}

				suggested = spell_dictionary->Correct(matches.front().get_keyword());
				return suggested.empty() ? matches.front().get_keyword() : suggested[0].term;
			};
		}

		inline std::function<std::wstring(const std::pair<cv::Rect, std::wstring>&)>
			create_extract_function(const std::wstring& regex)
		{
			return [&](const std::pair<cv::Rect, std::wstring>& line) -> std::wstring {
				boost::wregex re(regex, boost::regex_constants::icase);
				boost::match_results<std::wstring::const_iterator> matches;
				const auto text = std::get<1>(line);

				if (boost::regex_search(std::begin(text), std::end(text), matches, re)) {
					if (!matches.empty()) {
						for (auto i = 1; i < matches.size(); i++) {
							const auto matched = std::wstring(matches[i].first, matches[i].second);

							if (!matched.empty())
								return matched;
						}
					}
				}

				return L"";
			};
		}

		inline std::function<std::wstring(const std::pair<cv::Rect, std::wstring>&)>
			create_extract_sender(const std::wstring& regex)
		{
			return [&](const std::pair<cv::Rect, std::wstring>& line) -> std::wstring {
				boost::wregex re(regex, boost::regex_constants::icase);
				boost::match_results<std::wstring::const_iterator> matches;
				const auto text = std::get<1>(line);

				//text = std::to_wstring(boost::replace_all_copy(text, L"SWIFT MT SENDER", L""));

				if (boost::regex_search(std::begin(text), std::end(text), matches, re)) {
					if (!matches.empty()) {
						for (auto i = 1; i < matches.size(); i++) {
							const auto matched = std::wstring(matches[i].first, matches[i].second);
							
							if (!matched.empty())
								return matched;
						}
					}
				}

				return L"";
			};
		}

		inline std::function<std::wstring(const std::pair<cv::Rect, std::wstring>&)>
			create_coa_extract_function(const std::wstring& regex)
		{
			return [&](const std::pair<cv::Rect, std::wstring>& line) -> std::wstring {
				boost::wregex re(regex, boost::regex_constants::icase);
				boost::match_results<std::wstring::const_iterator> matches;
				const auto text = std::get<1>(line);

				if (boost::regex_search(std::begin(text), std::end(text), matches, re)) {
					if (!matches.empty()) {
						for (auto i = 0; i < matches.size(); i++) {
							const auto matched = std::wstring(matches[i].first, matches[i].second);

							if (!matched.empty())
								return matched;
						}
					}
				}

				return L"";
			};
		}


		class fax_document_recognizer : public recognizer
		{
		public:
			std::pair<std::wstring, int>
				recognize(const std::string& buffer, int languages, const std::string& secret) override
			{
				throw std::logic_error("not implmented");
			}
			std::pair<std::wstring, int>
				recognize(const std::string& buffer, const std::wstring& type, const std::string& secret) override
			{
				throw std::logic_error("not implmented");
			}
			std::unordered_map<std::wstring, std::vector<std::wstring>>
				recognize(const boost::filesystem::path& path,
				const std::wstring& class_name) override
			{
				const auto configuration = load_configuration(L"fax");

				std::unordered_map<std::wstring, std::vector<std::unordered_map<std::wstring, std::wstring>>> fields;

				std::mutex locks;
				const auto fill_field = [&locks](std::unordered_map<std::wstring, std::wstring>& fields, const std::wstring& field,
					const std::wstring& value) {
					locks.lock();
					if (!value.empty() && fields.find(field) == fields.end())
						fields.emplace(std::make_pair(field, value));
					locks.unlock();
				};

				std::vector<std::wstring> files;
				if (boost::filesystem::is_directory(path)) {
					for (auto& entry : boost::filesystem::recursive_directory_iterator(path)) {
						const auto file = entry.path();
						const auto extension = boost::algorithm::to_lower_copy(file.extension().native());

						if (extension != L".png" && extension != L".jpg" && extension != L".tif" && extension != L".jp2")
							continue;

						files.emplace_back(boost::filesystem::absolute(file).native());
					}

					std::sort(std::begin(files), std::end(files), compareNat);
				}
				else {
					const auto extension = boost::algorithm::to_lower_copy(path.extension().native());

					if (extension != L".png" && extension != L".jpg" && extension != L".tif" && extension != L".jp2")
						return std::unordered_map<std::wstring, std::vector<std::wstring>>();

					files.emplace_back(boost::filesystem::absolute(path).native());
				}

#ifdef LOG_USE_WOFSTREAM
				std::wofstream txt_file;
				txt_file.imbue(std::locale(std::locale::empty(), new std::codecvt_utf8<wchar_t, 0x10ffff, std::generate_header>));
				txt_file.open(__LOG_FILE_NAME__, std::wofstream::out | std::wofstream::app);
#endif

				// DB 정보
				// #01. 요구기관 : DEMAND_INSTI : CHAR : 40
				// #02. 정보요청기관구분 : INFO_REQ_INSTI_MK : CHAR : 2
				// #03. 우송료청구구분 : DLV_AMT_MK : CHAR : 2
				// #04. 문서번호 : DOC_NO : CHAR : 30
				// #05. 영장정보 : WARRNT_DOC_NO : CHAR : 30
				// #06. 요구담당자직책 : DMND_INCHRG_TLE : CHAR : 20
				// #07. 요구담당자성명 : DMND_INCHRG_NM : CHAR : 20
				// #08. 요구책임자성명 : DMND_CHRGP_NM : CHAR : 20
				// #09. 전화번호 : TELNO : CHAR : 14
				// #10. FAX번호 : FAX_NO : CHAR : 14
				// #11. 법적근거구분 : LEGAL_BASE_MK : CHAR : 2
				// #12. 사용목적코드 : USE_OBJCTV_CD : CHAR : 1
				// #13. 통보유예기간 : NOTICE_DTRRD_PRD : INT :
				// #14. 통보유예사유 : NOTICE_DTRRD_RSN : CHAR : 1
				// #15. 범죄명 : CRIME_NM : CHAR : 2
				// #16. E-MAIL : EMAIL_ID : CHAR : 80
				// #17. 통보문서양식 : DOC_TYPE : CHAR : 1
				// #18. OTHER1 : OTHER1 : CHAR : 80
				// #19. OTHER2 : OTHER2 : CHAR : 80
				// #20. OTHER3 : OTHER3 : CHAR : 80
				// #21. OTHER4 : OTHER4 : CHAR : 80
				// #22. OTHER5 : OTHER5 : CHAR : 80

				bool has_agreement = false;
				bool has_trial_cert = false;
				bool is_dri_set = false;
				const bool use_cache = __USE_CACHE__;
				try {
					//cv::parallel_for_(cv::Range(0, files.size()), [&](const cv::Range& range) {
						CComPtr<FREngine::IEngineLoader> loader;
						FREngine::IEnginePtr engine;
						std::tie(loader, engine) = get_engine_object(configuration);

						//auto classification_engine = engine->CreateClassificationEngine();
						//auto classification_model = classification_engine->
							//CreateModelFromFile(get_classification_model(configuration).c_str());

						//for (auto i = range.start; i < range.end; i++) {
						for (auto i = 0; i < files.size(); i++) {
							memory_reader memory_reader(boost::filesystem::absolute(files[i]), "");

							if (is_document_filtered(memory_reader.decrypted_)) {
								continue;
							}

							FREngine::IFRDocumentPtr document;
							if (!use_cache) {
								document = engine->CreateFRDocument();
								document->AddImageFileFromStream(&memory_reader, nullptr, nullptr, nullptr, boost::filesystem::path(files[i]).filename().native().c_str());
								auto page_preprocessing_params = engine->CreatePagePreprocessingParams();
								page_preprocessing_params->CorrectOrientation = VARIANT_TRUE;
								page_preprocessing_params->OrientationDetectionParams->put_OrientationDetectionMode(FREngine::OrientationDetectionModeEnum::ODM_Thorough);
								document->Preprocess(page_preprocessing_params, nullptr, nullptr, nullptr);

								if (document->Pages->Count < 1) {
									document->Close();
									continue;
								}
							}

#ifdef LOG_USE_WOFSTREAM
							txt_file << L"-----------------------------------------------------" << std::endl;
							txt_file << L"File : " << files[i] << std::endl;
#endif

							//const auto class_name = classify_document(engine, configuration, classification_engine, classification_model, files[i], document);
							if (class_name.empty()) {
								if (!use_cache) {
									document->Close();
								}
								continue;
							}

							const std::set<std::wstring> processed_class_names{
								L"Agreement Cover Page",
								L"Financial Transaction Information Request",
								L"Debt Repayment Inquiry",
								L"Bottom Contact Info Page",
								L"FTIR Next Page With Contact Info",
								L"Confiscation Search Verification Warrent",
								L"Certificate Of Trial",
								L"IDCard Of A PO With Contact Info",
								L"FTIR Next Page With Email",
								L"Cover Page Top Down Info",
								// L"Cover Page Tax Service Fax"
								// L"Cover Page Tax Service Logo",
								// L"Cover Page Police Agency Logo",
							};

							// 인식할 필요가 없는 문서는 pass
							if (processed_class_names.find(class_name) == processed_class_names.end()) {
								if (!use_cache) {
									document->Close();
								}
								continue;
							}

							// 세트에 동의서가 포함되었는지 여부
							if (class_name == L"Agreement Cover Page") {
								has_agreement = true;

								if (!use_cache) {
									document->Close();
								}
								continue;
							}

							if (class_name == L"Certificate Of Trial") {
								has_trial_cert = true;

								if (!use_cache) {
									document->Close();
								}
								continue;
							}

							if (class_name == L"Debt Repayment Inquiry")
								is_dri_set = true;

							std::vector<block> blocks;
							cv::Size image_size;
							std::tie(blocks, std::ignore, image_size) = recognize_document(engine, configuration, class_name, files[i], document);
							if (image_size.area() == 0)
								image_size = estimate_paper_size(blocks);

							if (!use_cache) {
								document->Close();
							}

							const auto keywords = get_keywords(configuration, keywords_, class_name);
							const auto searched_fields = search_fields(class_name, keywords, blocks);

							std::unordered_map<std::wstring, std::wstring> extracted_fields;

							if (class_name == L"Financial Transaction Information Request") {
								const auto request_organ = extract_request_organ(configuration, class_name, searched_fields, blocks, image_size);
								const auto document_number = extract_document_number(class_name, searched_fields, blocks, image_size);
								const auto grace_period = extract_grace_period(class_name, searched_fields, blocks, image_size);
								const auto warrant_number_value = extract_warrant_number(class_name, searched_fields, blocks, image_size);
								const auto warrant_number = std::get<1>(warrant_number_value);
								const auto use_purpose = extract_use_purpose(configuration, class_name, searched_fields, blocks, image_size);
								const auto telephone_number = extract_telephone_number(class_name, searched_fields, blocks, image_size, request_organ);
								const auto fax_number = extract_fax_number(class_name, searched_fields, blocks, telephone_number, image_size);
								const auto email_address = extract_email_address(class_name, searched_fields, blocks, image_size);
								auto manager_position_value = extract_manager_position(configuration, class_name, searched_fields, blocks, image_size, request_organ);;
								auto manager_position = std::get<1>(manager_position_value);
								const auto executive_name_value = extract_executive_name(class_name, searched_fields, blocks, image_size, request_organ);
								auto executive_name = std::get<1>(executive_name_value);
								const auto manager_name = extract_manager_name(class_name, searched_fields, blocks, manager_position_value, executive_name_value, image_size, request_organ);

								fill_field(extracted_fields, L"DEMAND_INSTI", request_organ);
								fill_field(extracted_fields, L"DOC_NO", document_number);
								fill_field(extracted_fields, L"NOTICE_DTRRD_PRD", grace_period);
								fill_field(extracted_fields, L"USE_OBJCTV_CD", use_purpose);
								fill_field(extracted_fields, L"WARRNT_DOC_NO", warrant_number);
								fill_field(extracted_fields, L"TELNO", telephone_number);
								fill_field(extracted_fields, L"FAX_NO", fax_number);
								fill_field(extracted_fields, L"EMAIL_ID", email_address);
								fill_field(extracted_fields, L"DMND_INCHRG_TLE", manager_position);
								fill_field(extracted_fields, L"DMND_INCHRG_NM", manager_name);
								fill_field(extracted_fields, L"DMND_CHRGP_NM", executive_name);
							}
							else if (class_name == L"Debt Repayment Inquiry") {
								const auto request_organ = extract_request_organ(configuration, class_name, searched_fields, blocks, image_size);
								const auto document_number = extract_document_number(class_name, searched_fields, blocks, image_size);
								const auto grace_period = extract_grace_period(class_name, searched_fields, blocks, image_size);
								const auto warrant_number_value = extract_warrant_number(class_name, searched_fields, blocks, image_size);
								const auto warrant_number = std::get<1>(warrant_number_value);
								const auto use_purpose = extract_use_purpose(configuration, class_name, searched_fields, blocks, image_size);
								const auto telephone_number = extract_telephone_number(class_name, searched_fields, blocks, image_size, request_organ);
								const auto fax_number = extract_fax_number(class_name, searched_fields, blocks, telephone_number, image_size);
								const auto email_address = extract_email_address(class_name, searched_fields, blocks, image_size);
								auto manager_position_value = extract_manager_position(configuration, class_name, searched_fields, blocks, image_size, request_organ);
								auto manager_position = std::get<1>(manager_position_value);
								const auto executive_name_value = extract_executive_name(class_name, searched_fields, blocks, image_size, request_organ);
								auto executive_name = std::get<1>(executive_name_value);
								const auto manager_name = extract_manager_name(class_name, searched_fields, blocks, manager_position_value, executive_name_value, image_size, request_organ);

								if (!manager_name.empty() && executive_name.empty()) {
									executive_name = manager_name;
								}

								fill_field(extracted_fields, L"DEMAND_INSTI", request_organ);
								fill_field(extracted_fields, L"DOC_NO", document_number);
								fill_field(extracted_fields, L"NOTICE_DTRRD_PRD", grace_period);
								fill_field(extracted_fields, L"USE_OBJCTV_CD", use_purpose);
								fill_field(extracted_fields, L"WARRNT_DOC_NO", warrant_number);
								fill_field(extracted_fields, L"TELNO", telephone_number);
								fill_field(extracted_fields, L"FAX_NO", fax_number);
								fill_field(extracted_fields, L"EMAIL_ID", email_address);
								fill_field(extracted_fields, L"DMND_INCHRG_TLE", manager_position);
								fill_field(extracted_fields, L"DMND_INCHRG_NM", manager_name);
								fill_field(extracted_fields, L"DMND_INCHRG_NM", executive_name);
							}
							else if (class_name == L"Confiscation Search Verification Warrent") {
								const auto warrant_number_value = extract_warrant_number(class_name, searched_fields, blocks, image_size);
								const auto warrant_number = std::get<1>(warrant_number_value);
								const auto crime_name = extract_crime_name(configuration, class_name, searched_fields, blocks, warrant_number_value, image_size);

								fill_field(extracted_fields, L"WARRNT_DOC_NO", warrant_number);
								fill_field(extracted_fields, L"CRIME_NM", crime_name);
							}
							else if (class_name == L"Bottom Contact Info Page") {
								const auto telephone_number = extract_telephone_number(class_name, searched_fields, blocks, image_size, L"");
								const auto fax_number = extract_fax_number(class_name, searched_fields, blocks, telephone_number, image_size);
								const auto email_address = extract_email_address(class_name, searched_fields, blocks, image_size);

								fill_field(extracted_fields, L"TELNO", telephone_number);
								fill_field(extracted_fields, L"FAX_NO", fax_number);
								fill_field(extracted_fields, L"EMAIL_ID", email_address);
							}
							else if (class_name == L"FTIR Next Page With Contact Info") {
								const auto telephone_number = extract_telephone_number(class_name, searched_fields, blocks, image_size, L"");
								const auto fax_number = extract_fax_number(class_name, searched_fields, blocks, telephone_number, image_size);
								const auto email_address = extract_email_address(class_name, searched_fields, blocks, image_size);
								const auto manager_name = extract_manager_name(class_name, searched_fields, blocks, std::make_pair(cv::Rect(), L""), std::make_pair(cv::Rect(), L""), image_size, L"");

								fill_field(extracted_fields, L"TELNO", telephone_number);
								fill_field(extracted_fields, L"FAX_NO", fax_number);
								fill_field(extracted_fields, L"EMAIL_ID", email_address);
								fill_field(extracted_fields, L"DMND_INCHRG_NM", manager_name);
							}
							else if (class_name == L"IDCard Of A PO With Contact Info") {
								const auto request_organ = extract_request_organ(configuration, class_name, searched_fields, blocks, image_size);
								const auto telephone_number = extract_telephone_number(class_name, searched_fields, blocks, image_size, L"");
								const auto fax_number = extract_fax_number(class_name, searched_fields, blocks, telephone_number, image_size);
								const auto email_address = extract_email_address(class_name, searched_fields, blocks, image_size);
								auto manager_position_value = extract_manager_position(configuration, class_name, searched_fields, blocks, image_size, request_organ);
								auto manager_position = std::get<1>(manager_position_value);
								const auto manager_name = extract_manager_name(class_name, searched_fields, blocks, manager_position_value, std::make_pair(cv::Rect(), L""), image_size, request_organ);

								fill_field(extracted_fields, L"DEMAND_INSTI", request_organ);
								fill_field(extracted_fields, L"TELNO", telephone_number);
								fill_field(extracted_fields, L"FAX_NO", fax_number);
								fill_field(extracted_fields, L"EMAIL_ID", email_address);
								fill_field(extracted_fields, L"DMND_INCHRG_TLE", manager_position);
								fill_field(extracted_fields, L"DMND_INCHRG_NM", manager_name);
							}
							else if (class_name == L"FTIR Next Page With Email") {
								const auto email_address = extract_email_address(class_name, searched_fields, blocks, image_size);

								fill_field(extracted_fields, L"EMAIL_ID", email_address);
							}
							else if (class_name == L"Cover Page Top Down Info") {
								const auto request_organ = extract_request_organ(configuration, class_name, searched_fields, blocks, image_size);
								const auto telephone_number = extract_telephone_number(class_name, searched_fields, blocks, image_size, L"");
								const auto fax_number = extract_fax_number(class_name, searched_fields, blocks, telephone_number, image_size);
								const auto email_address = extract_email_address(class_name, searched_fields, blocks, image_size);

								fill_field(extracted_fields, L"DEMAND_INSTI", request_organ);
								fill_field(extracted_fields, L"TELNO", telephone_number);
								fill_field(extracted_fields, L"FAX_NO", fax_number);
								fill_field(extracted_fields, L"EMAIL_ID", email_address);
							}

#if defined(_DEBUG)
							const auto image = debug_;
#endif

#ifdef LOG_USE_WOFSTREAM            // 문서단위 결과
							const auto text = to_wstring(blocks);
							txt_file << L"ALL : " << std::endl;
							txt_file << text << std::endl << std::endl;

							txt_file << L"- 파일 : " << files[i] << std::endl;
							txt_file << L"- 분류 : " << class_name << std::endl;

							txt_file << "- " << convert_field_column_to_string(L"DEMAND_INSTI") << " : " << get_field_value(extracted_fields, L"DEMAND_INSTI") << std::endl;
							txt_file << "- " << convert_field_column_to_string(L"DOC_NO") << " : " << get_field_value(extracted_fields, L"DOC_NO") << std::endl;
							txt_file << "- " << convert_field_column_to_string(L"NOTICE_DTRRD_PRD") << " : " << get_field_value(extracted_fields, L"NOTICE_DTRRD_PRD") << std::endl;
							txt_file << "- " << convert_field_column_to_string(L"USE_OBJCTV_CD") << " : " << get_field_value(extracted_fields, L"USE_OBJCTV_CD") << std::endl;
							txt_file << "- " << convert_field_column_to_string(L"WARRNT_DOC_NO") << " : " << get_field_value(extracted_fields, L"WARRNT_DOC_NO") << std::endl;
							txt_file << "- " << convert_field_column_to_string(L"CRIME_NM") << " : " << get_field_value(extracted_fields, L"CRIME_NM") << std::endl;
							txt_file << "- " << convert_field_column_to_string(L"TELNO") << " : " << get_field_value(extracted_fields, L"TELNO") << std::endl;
							txt_file << "- " << convert_field_column_to_string(L"FAX_NO") << " : " << get_field_value(extracted_fields, L"FAX_NO") << std::endl;
							txt_file << "- " << convert_field_column_to_string(L"EMAIL_ID") << " : " << get_field_value(extracted_fields, L"EMAIL_ID") << std::endl;
							txt_file << "- " << convert_field_column_to_string(L"DMND_INCHRG_TLE") << " : " << get_field_value(extracted_fields, L"DMND_INCHRG_TLE") << std::endl;
							txt_file << "- " << convert_field_column_to_string(L"DMND_INCHRG_NM") << " : " << get_field_value(extracted_fields, L"DMND_INCHRG_NM") << std::endl;
							txt_file << "- " << convert_field_column_to_string(L"DMND_CHRGP_NM") << " : " << get_field_value(extracted_fields, L"DMND_CHRGP_NM") << std::endl;
#endif

							if (extracted_fields.empty())
								continue;

							if (fields.find(class_name) == fields.end())
								fields.emplace(class_name, std::vector<std::unordered_map<std::wstring, std::wstring>>());

							fields.at(class_name).emplace_back(extracted_fields);
						}

						release_engine_object(std::make_pair(loader, engine));
					//}, std::stoi(configuration.at(L"engine").at(L"concurrency")));
				}
				catch (_com_error& e) {
					spdlog::get("recognizer")->error("exception : {} : ({} : {})", to_cp949(e.Description().GetBSTR()), __FILE__, __LINE__);
				}

				const std::unordered_map<std::wstring, std::wstring> db_default_value = {
					{ L"DEMAND_INSTI", DEFAULT_FAX_COLUMN_VALUE_FOR_STRING },
					{ L"INFO_REQ_INSTI_MK", DEFAULT_FAX_COLUMN_VALUE_FOR_CODE },
					{ L"DLV_AMT_MK", DEFAULT_FAX_COLUMN_VALUE_FOR_STRING },
					{ L"DOC_NO", DEFAULT_FAX_COLUMN_VALUE_FOR_STRING },
					{ L"WARRNT_DOC_NO", DEFAULT_FAX_COLUMN_VALUE_FOR_STRING },
					{ L"DMND_INCHRG_TLE", DEFAULT_FAX_COLUMN_VALUE_FOR_STRING },
					{ L"DMND_INCHRG_NM", DEFAULT_FAX_COLUMN_VALUE_FOR_STRING },
					{ L"DMND_CHRGP_NM", DEFAULT_FAX_COLUMN_VALUE_FOR_STRING },
					{ L"TELNO", DEFAULT_FAX_COLUMN_VALUE_FOR_STRING },
					{ L"FAX_NO", DEFAULT_FAX_COLUMN_VALUE_FOR_STRING },
					{ L"LEGAL_BASE_MK", DEFAULT_FAX_COLUMN_VALUE_FOR_CODE },
					{ L"USE_OBJCTV_CD", DEFAULT_FAX_COLUMN_VALUE_FOR_CODE },
					{ L"NOTICE_DTRRD_PRD", DEFAULT_FAX_COLUMN_VALUE_FOR_PRD },
					{ L"NOTICE_DTRRD_RSN", DEFAULT_FAX_COLUMN_VALUE_FOR_CODE },
					{ L"CRIME_NM", DEFAULT_FAX_COLUMN_VALUE_FOR_CODE },
					{ L"EMAIL_ID", DEFAULT_FAX_COLUMN_VALUE_FOR_STRING },
					{ L"DOC_TYPE", DEFAULT_FAX_COLUMN_VALUE_FOR_CODE },
					{ L"OTHER1", DEFAULT_FAX_COLUMN_VALUE_FOR_OTHER },
					{ L"OTHER2", DEFAULT_FAX_COLUMN_VALUE_FOR_OTHER },
					{ L"OTHER3", DEFAULT_FAX_COLUMN_VALUE_FOR_OTHER },
					{ L"OTHER4", DEFAULT_FAX_COLUMN_VALUE_FOR_OTHER },
					{ L"OTHER5", DEFAULT_FAX_COLUMN_VALUE_FOR_OTHER },
				};

				std::unordered_map<std::wstring, std::vector<std::wstring>> db_fields = {
					{ L"DEMAND_INSTI", { DEFAULT_FAX_COLUMN_VALUE_FOR_STRING } },
					{ L"INFO_REQ_INSTI_MK", { DEFAULT_FAX_COLUMN_VALUE_FOR_CODE } },
					{ L"DLV_AMT_MK", { DEFAULT_FAX_COLUMN_VALUE_FOR_CODE } },
					{ L"DOC_NO", { DEFAULT_FAX_COLUMN_VALUE_FOR_STRING } },
					{ L"WARRNT_DOC_NO", { DEFAULT_FAX_COLUMN_VALUE_FOR_STRING } },
					{ L"DMND_INCHRG_TLE", { DEFAULT_FAX_COLUMN_VALUE_FOR_STRING } },
					{ L"DMND_INCHRG_NM", { DEFAULT_FAX_COLUMN_VALUE_FOR_STRING } },
					{ L"DMND_CHRGP_NM", { DEFAULT_FAX_COLUMN_VALUE_FOR_STRING } },
					{ L"TELNO", { DEFAULT_FAX_COLUMN_VALUE_FOR_STRING } },
					{ L"FAX_NO", { DEFAULT_FAX_COLUMN_VALUE_FOR_STRING } },
					{ L"LEGAL_BASE_MK", { DEFAULT_FAX_COLUMN_VALUE_FOR_CODE } },
					{ L"USE_OBJCTV_CD", { DEFAULT_FAX_COLUMN_VALUE_FOR_CODE } },
					{ L"NOTICE_DTRRD_PRD", { DEFAULT_FAX_COLUMN_VALUE_FOR_PRD } },
					{ L"NOTICE_DTRRD_RSN", { DEFAULT_FAX_COLUMN_VALUE_FOR_CODE } },
					{ L"CRIME_NM", { DEFAULT_FAX_COLUMN_VALUE_FOR_CODE } },
					{ L"EMAIL_ID", { DEFAULT_FAX_COLUMN_VALUE_FOR_STRING } },
					{ L"DOC_TYPE", { DEFAULT_FAX_COLUMN_VALUE_FOR_CODE } },
					{ L"OTHER1", { DEFAULT_FAX_COLUMN_VALUE_FOR_OTHER } },
					{ L"OTHER2", { DEFAULT_FAX_COLUMN_VALUE_FOR_OTHER } },
					{ L"OTHER3", { DEFAULT_FAX_COLUMN_VALUE_FOR_OTHER } },
					{ L"OTHER4", { DEFAULT_FAX_COLUMN_VALUE_FOR_OTHER } },
					{ L"OTHER5", { DEFAULT_FAX_COLUMN_VALUE_FOR_OTHER } },
				};

				const std::vector<std::wstring> ordered_categories = {
					L"Financial Transaction Information Request",
					L"Debt Repayment Inquiry",
					L"Confiscation Search Verification Warrent",
					L"Bottom Contact Info Page",
					L"FTIR Next Page With Contact Info",
					L"IDCard Of A PO With Contact Info",
					L"FTIR Next Page With Email",
					L"Cover Page Top Down Info",
					// L"Cover Page Tax Service Fax",
					// L"Cover Page Tax Service Logo",
					// L"Cover Page Police Agency Logo",
				};

				// 인식된 값을 넣는다. 0번 Index 만 사용!!!
				for (const auto& category : ordered_categories) {
					if (fields.find(category) != fields.end() && !fields.at(category).empty()) {
						const auto field_map = fields.at(category)[0];

						for (const auto& field : field_map) {
							if (!std::get<1>(field).empty()
								&& db_fields.at(std::get<0>(field))[0] == db_default_value.at(std::get<0>(field))) {
								db_fields.at(std::get<0>(field))[0] = std::get<1>(field);
							}
						}
					}
				}

				// 통보유예기간의 경우 인식된 값이 없으면 default 0 을 사용
				/*if (db_fields.at(L"NOTICE_DTRRD_PRD")[0] == DEFAULT_FAX_COLUMN_VALUE)
				db_fields.at(L"NOTICE_DTRRD_PRD")[0] = L"0",*/

				// 담당자 직책과 이름이 같을 경우 오인식이므로 처리
				if (db_fields.at(L"DMND_INCHRG_TLE")[0] == db_fields.at(L"DMND_INCHRG_NM")[0])
					db_fields.at(L"DMND_INCHRG_TLE")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_STRING;

				const auto request_organ = db_fields.at(L"DEMAND_INSTI")[0];
				if (request_organ.find(L"국세청") != std::wstring::npos || request_organ.find(L"세무서") != std::wstring::npos) {
					if (!has_agreement) {
						db_fields.at(L"INFO_REQ_INSTI_MK")[0] = L"15";
						db_fields.at(L"DLV_AMT_MK")[0] = L"03";
						db_fields.at(L"LEGAL_BASE_MK")[0] = L"03";
						if (db_fields.at(L"USE_OBJCTV_CD")[0] == L"상속세" || db_fields.at(L"USE_OBJCTV_CD")[0] == L"증여세")
							db_fields.at(L"USE_OBJCTV_CD")[0] = L"3";
						else
							db_fields.at(L"USE_OBJCTV_CD")[0] = L"2";
						db_fields.at(L"CRIME_NM")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
						db_fields.at(L"NOTICE_DTRRD_RSN")[0] = L"2";
						db_fields.at(L"DOC_TYPE")[0] = L"1";
					}
					else {
						db_fields.at(L"INFO_REQ_INSTI_MK")[0] = L"01";
						db_fields.at(L"DLV_AMT_MK")[0] = L"05";
						db_fields.at(L"LEGAL_BASE_MK")[0] = L"01";
						db_fields.at(L"USE_OBJCTV_CD")[0] = L"2";
						db_fields.at(L"CRIME_NM")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
						db_fields.at(L"NOTICE_DTRRD_RSN")[0] = L"2";
						db_fields.at(L"DOC_TYPE")[0] = L"1";
					}
				}
				else if (request_organ.find(L"관세청") != std::wstring::npos || request_organ.find(L"세관") != std::wstring::npos) {
					db_fields.at(L"INFO_REQ_INSTI_MK")[0] = L"14";
					db_fields.at(L"DLV_AMT_MK")[0] = L"02";
					db_fields.at(L"LEGAL_BASE_MK")[0] = L"08";
					db_fields.at(L"USE_OBJCTV_CD")[0] = L"2";
					db_fields.at(L"CRIME_NM")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
					db_fields.at(L"NOTICE_DTRRD_RSN")[0] = L"2";
					db_fields.at(L"DOC_TYPE")[0] = L"1";
				}
				else if (request_organ.find(L"검찰청") != std::wstring::npos) {
					if (!has_agreement) {
						db_fields.at(L"INFO_REQ_INSTI_MK")[0] = L"02";
						db_fields.at(L"DLV_AMT_MK")[0] = L"04";
						db_fields.at(L"LEGAL_BASE_MK")[0] = L"02";
						db_fields.at(L"USE_OBJCTV_CD")[0] = L"1";
						if (!has_trial_cert)
							db_fields.at(L"CRIME_NM")[0] = convert_crime_name_to_code(db_fields.at(L"CRIME_NM")[0]);
						else
							db_fields.at(L"CRIME_NM")[0] = L"07";
						db_fields.at(L"NOTICE_DTRRD_RSN")[0] = L"1";
						db_fields.at(L"DOC_TYPE")[0] = L"1";
					}
					else {
						db_fields.at(L"INFO_REQ_INSTI_MK")[0] = L"01";
						db_fields.at(L"DLV_AMT_MK")[0] = L"05";
						db_fields.at(L"LEGAL_BASE_MK")[0] = L"01";
						db_fields.at(L"USE_OBJCTV_CD")[0] = L"1";
						db_fields.at(L"CRIME_NM")[0] = L"14";
						db_fields.at(L"NOTICE_DTRRD_RSN")[0] = L"1";
						db_fields.at(L"DOC_TYPE")[0] = L"1";
					}
				}
				else if (request_organ.find(L"경찰서") != std::wstring::npos) {
					if (!has_agreement) {
						db_fields.at(L"INFO_REQ_INSTI_MK")[0] = L"02";
						db_fields.at(L"DLV_AMT_MK")[0] = L"01";
						db_fields.at(L"LEGAL_BASE_MK")[0] = L"02";
						db_fields.at(L"USE_OBJCTV_CD")[0] = L"1";
						if (!has_trial_cert)
							db_fields.at(L"CRIME_NM")[0] = convert_crime_name_to_code(db_fields.at(L"CRIME_NM")[0]);
						else
							db_fields.at(L"CRIME_NM")[0] = L"07";
						db_fields.at(L"NOTICE_DTRRD_RSN")[0] = L"1";
						db_fields.at(L"DOC_TYPE")[0] = L"1";
					}
					else {
						db_fields.at(L"INFO_REQ_INSTI_MK")[0] = L"01";
						db_fields.at(L"DLV_AMT_MK")[0] = L"05";
						db_fields.at(L"LEGAL_BASE_MK")[0] = L"01";
						db_fields.at(L"USE_OBJCTV_CD")[0] = L"1";
						db_fields.at(L"CRIME_NM")[0] = L"14";
						db_fields.at(L"NOTICE_DTRRD_RSN")[0] = L"1";
						db_fields.at(L"DOC_TYPE")[0] = L"1";
					}
				}
				else if (request_organ.find(L"경찰청") != std::wstring::npos) {
					db_fields.at(L"INFO_REQ_INSTI_MK")[0] = L"02";
					db_fields.at(L"DLV_AMT_MK")[0] = L"01";
					db_fields.at(L"LEGAL_BASE_MK")[0] = L"02";
					db_fields.at(L"USE_OBJCTV_CD")[0] = L"1";
					db_fields.at(L"CRIME_NM")[0] = convert_crime_name_to_code(db_fields.at(L"CRIME_NM")[0]);
					db_fields.at(L"NOTICE_DTRRD_RSN")[0] = L"1";
					db_fields.at(L"DOC_TYPE")[0] = L"1";
				}
				else if (request_organ.find(L"금융감독원") != std::wstring::npos) {
					db_fields.at(L"INFO_REQ_INSTI_MK")[0] = L"04";
					db_fields.at(L"DLV_AMT_MK")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
					db_fields.at(L"LEGAL_BASE_MK")[0] = L"12";
					db_fields.at(L"USE_OBJCTV_CD")[0] = L"5";
					db_fields.at(L"CRIME_NM")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
					db_fields.at(L"NOTICE_DTRRD_RSN")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
					db_fields.at(L"DOC_TYPE")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
				}
				else if (request_organ.find(L"예금보험공사") != std::wstring::npos) {
					db_fields.at(L"INFO_REQ_INSTI_MK")[0] = L"05";
					db_fields.at(L"DLV_AMT_MK")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
					db_fields.at(L"LEGAL_BASE_MK")[0] = L"09";
					db_fields.at(L"USE_OBJCTV_CD")[0] = L"5";
					db_fields.at(L"CRIME_NM")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
					db_fields.at(L"NOTICE_DTRRD_RSN")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
					db_fields.at(L"DOC_TYPE")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
				}
				else if (request_organ.find(L"은행") != std::wstring::npos || request_organ.find(L"금고") != std::wstring::npos
					|| request_organ.find(L"뱅크") != std::wstring::npos || request_organ.find(L"에셋") != std::wstring::npos
					|| request_organ.find(L"증권") != std::wstring::npos || request_organ.find(L"조합") != std::wstring::npos
					|| request_organ.find(L"수협") != std::wstring::npos || request_organ.find(L"신협") != std::wstring::npos
					|| request_organ.find(L"투자") != std::wstring::npos || request_organ.find(L"우정사업본부") != std::wstring::npos
					|| request_organ.find(L"농협") != std::wstring::npos) {
					db_fields.at(L"INFO_REQ_INSTI_MK")[0] = L"99";
					db_fields.at(L"DLV_AMT_MK")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
					db_fields.at(L"LEGAL_BASE_MK")[0] = L"11";
					db_fields.at(L"USE_OBJCTV_CD")[0] = L"5";
					db_fields.at(L"CRIME_NM")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
					db_fields.at(L"NOTICE_DTRRD_RSN")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
					db_fields.at(L"DOC_TYPE")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
				}
				else if (request_organ.find(L"보훈청") != std::wstring::npos || request_organ.find(L"보훈지청") != std::wstring::npos
					|| request_organ.find(L"병무청") != std::wstring::npos || request_organ.find(L"병무지청") != std::wstring::npos) {
					if (!has_agreement) {// ???
						db_fields.at(L"INFO_REQ_INSTI_MK")[0] = L"03";
						db_fields.at(L"DLV_AMT_MK")[0] = L"05";
						db_fields.at(L"LEGAL_BASE_MK")[0] = L"07";
						db_fields.at(L"USE_OBJCTV_CD")[0] = L"5";
						db_fields.at(L"CRIME_NM")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
						db_fields.at(L"NOTICE_DTRRD_RSN")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
						db_fields.at(L"DOC_TYPE")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
					}
					else {
						db_fields.at(L"INFO_REQ_INSTI_MK")[0] = L"01";
						db_fields.at(L"DLV_AMT_MK")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
						db_fields.at(L"LEGAL_BASE_MK")[0] = L"01";
						db_fields.at(L"USE_OBJCTV_CD")[0] = L"5";
						db_fields.at(L"CRIME_NM")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
						db_fields.at(L"NOTICE_DTRRD_RSN")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
						db_fields.at(L"DOC_TYPE")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
					}
				}
				else if (request_organ.find(L"감사원") != std::wstring::npos) {
					if (!has_agreement) {
						db_fields.at(L"INFO_REQ_INSTI_MK")[0] = L"06";
						db_fields.at(L"DLV_AMT_MK")[0] = L"05";
						db_fields.at(L"LEGAL_BASE_MK")[0] = L"13";
						db_fields.at(L"USE_OBJCTV_CD")[0] = L"5";
						db_fields.at(L"CRIME_NM")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
						db_fields.at(L"NOTICE_DTRRD_RSN")[0] = L"1";
						db_fields.at(L"DOC_TYPE")[0] = L"1";
					}
					else {
						db_fields.at(L"INFO_REQ_INSTI_MK")[0] = L"01";
						db_fields.at(L"DLV_AMT_MK")[0] = L"05";
						db_fields.at(L"LEGAL_BASE_MK")[0] = L"01";
						db_fields.at(L"USE_OBJCTV_CD")[0] = L"5";
						db_fields.at(L"CRIME_NM")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
						db_fields.at(L"NOTICE_DTRRD_RSN")[0] = L"1";
						db_fields.at(L"DOC_TYPE")[0] = L"1";
					}
				}
				else if (request_organ.find_last_of(L"시") != std::wstring::npos && request_organ.find_last_of(L"시") == request_organ.size() - 1) {
					db_fields.at(L"INFO_REQ_INSTI_MK")[0] = L"03";
					db_fields.at(L"DLV_AMT_MK")[0] = L"05";
					db_fields.at(L"LEGAL_BASE_MK")[0] = L"07";
					db_fields.at(L"USE_OBJCTV_CD")[0] = L"2";
					db_fields.at(L"CRIME_NM")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
					db_fields.at(L"NOTICE_DTRRD_RSN")[0] = L"2";
					db_fields.at(L"DOC_TYPE")[0] = L"1";
				}
				else {
					db_fields.at(L"CRIME_NM")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
				}

				if (is_dri_set) {
					db_fields.at(L"INFO_REQ_INSTI_MK")[0] = L"15";
					db_fields.at(L"DLV_AMT_MK")[0] = L"03";
					db_fields.at(L"LEGAL_BASE_MK")[0] = L"03";
					db_fields.at(L"USE_OBJCTV_CD")[0] = L"2";
					db_fields.at(L"CRIME_NM")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
					db_fields.at(L"NOTICE_DTRRD_PRD")[0] = L"0";
					db_fields.at(L"NOTICE_DTRRD_RSN")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
					db_fields.at(L"DOC_TYPE")[0] = L"1";

					if (db_fields.at(L"DMND_CHRGP_NM")[0] == DEFAULT_FAX_COLUMN_VALUE_FOR_STRING)
						db_fields.at(L"DMND_CHRGP_NM")[0] == db_fields.at(L"DMND_INCHRG_NM")[0];

					if (db_fields.at(L"DMND_INCHRG_TLE")[0] == DEFAULT_FAX_COLUMN_VALUE_FOR_STRING)
						db_fields.at(L"DMND_INCHRG_TLE")[0] = L"국세조사관";

				}

#ifdef LOG_USE_WOFSTREAM
				txt_file << L"=====================================================" << std::endl;

				txt_file << "= " << convert_field_column_to_string(L"DEMAND_INSTI") << " : " << db_fields.at(L"DEMAND_INSTI")[0] << std::endl;
				txt_file << "= " << convert_field_column_to_string(L"INFO_REQ_INSTI_MK") << " : " << convert_info_req_insti_mk_to_string(db_fields.at(L"INFO_REQ_INSTI_MK")[0]) << std::endl;
				txt_file << "= " << convert_field_column_to_string(L"DLV_AMT_MK") << " : " << convert_dlv_amt_mk_to_string(db_fields.at(L"DLV_AMT_MK")[0]) << std::endl;
				txt_file << "= " << convert_field_column_to_string(L"LEGAL_BASE_MK") << " : " << convert_legal_base_mk_to_string(db_fields.at(L"LEGAL_BASE_MK")[0]) << std::endl;
				txt_file << "= " << convert_field_column_to_string(L"USE_OBJCTV_CD") << " : " << convert_use_objctv_cd_to_string(db_fields.at(L"USE_OBJCTV_CD")[0]) << std::endl;
				txt_file << "= " << convert_field_column_to_string(L"CRIME_NM") << " : " << convert_crime_nm_to_string(db_fields.at(L"CRIME_NM")[0]) << std::endl;
				txt_file << "= " << convert_field_column_to_string(L"DOC_TYPE") << " : " << convert_notice_doc_type_to_string(db_fields.at(L"DOC_TYPE")[0]) << std::endl;
				txt_file << "= " << convert_field_column_to_string(L"NOTICE_DTRRD_RSN") << " : " << convert_notice_dtrrd_rsn_to_string(db_fields.at(L"NOTICE_DTRRD_RSN")[0]) << std::endl;

				txt_file << "= " << convert_field_column_to_string(L"DOC_NO") << " : " << db_fields.at(L"DOC_NO")[0] << std::endl;
				txt_file << "= " << convert_field_column_to_string(L"WARRNT_DOC_NO") << " : " << db_fields.at(L"WARRNT_DOC_NO")[0] << std::endl;
				txt_file << "= " << convert_field_column_to_string(L"NOTICE_DTRRD_PRD") << " : " << db_fields.at(L"NOTICE_DTRRD_PRD")[0] << std::endl;
				txt_file << "= " << convert_field_column_to_string(L"TELNO") << " : " << db_fields.at(L"TELNO")[0] << std::endl;
				txt_file << "= " << convert_field_column_to_string(L"FAX_NO") << " : " << db_fields.at(L"FAX_NO")[0] << std::endl;
				txt_file << "= " << convert_field_column_to_string(L"EMAIL_ID") << " : " << db_fields.at(L"EMAIL_ID")[0] << std::endl;
				txt_file << "= " << convert_field_column_to_string(L"DMND_INCHRG_TLE") << " : " << db_fields.at(L"DMND_INCHRG_TLE")[0] << std::endl;
				txt_file << "= " << convert_field_column_to_string(L"DMND_INCHRG_NM") << " : " << db_fields.at(L"DMND_INCHRG_NM")[0] << std::endl;
				txt_file << "= " << convert_field_column_to_string(L"DMND_CHRGP_NM") << " : " << db_fields.at(L"DMND_CHRGP_NM")[0] << std::endl;

				txt_file << "= " << convert_field_column_to_string(L"OTHER1") << " : " << db_fields.at(L"OTHER1")[0] << std::endl;
				txt_file << "= " << convert_field_column_to_string(L"OTHER2") << " : " << db_fields.at(L"OTHER2")[0] << std::endl;
				txt_file << "= " << convert_field_column_to_string(L"OTHER3") << " : " << db_fields.at(L"OTHER3")[0] << std::endl;
				txt_file << "= " << convert_field_column_to_string(L"OTHER4") << " : " << db_fields.at(L"OTHER4")[0] << std::endl;
				txt_file << "= " << convert_field_column_to_string(L"OTHER5") << " : " << db_fields.at(L"OTHER5")[0] << std::endl;

				txt_file << L"=====================================================" << std::endl;

				txt_file.close();
#endif

				return db_fields;
			}

			std::unordered_map<std::wstring, std::vector<std::wstring>>
				recognize(const boost::filesystem::path& path, const std::string& secret) override
			{
				const auto configuration = load_configuration(L"fax");

				std::unordered_map<std::wstring, std::vector<std::unordered_map<std::wstring, std::wstring>>> fields;

				std::mutex locks;
				const auto fill_field = [&locks](std::unordered_map<std::wstring, std::wstring>& fields, const std::wstring& field,
					const std::wstring& value) {
					locks.lock();
					if (!value.empty() && fields.find(field) == fields.end())
						fields.emplace(std::make_pair(field, value));
					locks.unlock();
				};

				std::vector<std::wstring> files;
				if (boost::filesystem::is_directory(path)) {
					for (auto& entry : boost::filesystem::recursive_directory_iterator(path)) {
						const auto file = entry.path();
						const auto extension = boost::algorithm::to_lower_copy(file.extension().native());

						if (extension != L".png" && extension != L".jpg" && extension != L".tif" && extension != L".jp2")
							continue;

						files.emplace_back(boost::filesystem::absolute(file).native());
					}

					std::sort(std::begin(files), std::end(files), compareNat);
				}
				else {
					const auto extension = boost::algorithm::to_lower_copy(path.extension().native());

					if (extension != L".png" && extension != L".jpg" && extension != L".tif" && extension != L".jp2")
						return std::unordered_map<std::wstring, std::vector<std::wstring>>();

					files.emplace_back(boost::filesystem::absolute(path).native());
				}

#ifdef LOG_USE_WOFSTREAM
				std::wofstream txt_file;
				txt_file.imbue(std::locale(std::locale::empty(), new std::codecvt_utf8<wchar_t, 0x10ffff, std::generate_header>));
				txt_file.open(__LOG_FILE_NAME__, std::wofstream::out | std::wofstream::app);
#endif

				// DB 정보
				// #01. 요구기관 : DEMAND_INSTI : CHAR : 40
				// #02. 정보요청기관구분 : INFO_REQ_INSTI_MK : CHAR : 2
				// #03. 우송료청구구분 : DLV_AMT_MK : CHAR : 2
				// #04. 문서번호 : DOC_NO : CHAR : 30
				// #05. 영장정보 : WARRNT_DOC_NO : CHAR : 30
				// #06. 요구담당자직책 : DMND_INCHRG_TLE : CHAR : 20
				// #07. 요구담당자성명 : DMND_INCHRG_NM : CHAR : 20
				// #08. 요구책임자성명 : DMND_CHRGP_NM : CHAR : 20
				// #09. 전화번호 : TELNO : CHAR : 14
				// #10. FAX번호 : FAX_NO : CHAR : 14
				// #11. 법적근거구분 : LEGAL_BASE_MK : CHAR : 2
				// #12. 사용목적코드 : USE_OBJCTV_CD : CHAR : 1
				// #13. 통보유예기간 : NOTICE_DTRRD_PRD : INT :
				// #14. 통보유예사유 : NOTICE_DTRRD_RSN : CHAR : 1
				// #15. 범죄명 : CRIME_NM : CHAR : 2
				// #16. E-MAIL : EMAIL_ID : CHAR : 80
				// #17. 통보문서양식 : DOC_TYPE : CHAR : 1
				// #18. OTHER1 : OTHER1 : CHAR : 80
				// #19. OTHER2 : OTHER2 : CHAR : 80
				// #20. OTHER3 : OTHER3 : CHAR : 80
				// #21. OTHER4 : OTHER4 : CHAR : 80
				// #22. OTHER5 : OTHER5 : CHAR : 80

				bool has_agreement = false;
				bool has_trial_cert = false;
				bool is_dri_set = false;
				try {
					// cv::parallel_for_(cv::Range(0, files.size()), [&](const cv::Range& range) {
						CComPtr<FREngine::IEngineLoader> loader;
						FREngine::IEnginePtr engine;
						std::tie(loader, engine) = get_engine_object(configuration);

						auto classification_engine = engine->CreateClassificationEngine();
						auto classification_model = classification_engine->
							CreateModelFromFile(get_classification_model(configuration).c_str());

						// for (auto i = range.start; i < range.end; i++) {
						for (auto i = 0; i < files.size(); i++) {
							memory_reader memory_reader(boost::filesystem::absolute(files[i]), secret);

							if (is_document_filtered(memory_reader.decrypted_)) {
								continue;
							}

							auto document = engine->CreateFRDocument();
							document->AddImageFileFromStream(&memory_reader, nullptr, nullptr, nullptr, boost::filesystem::path(files[i]).filename().native().c_str());
							auto page_preprocessing_params = engine->CreatePagePreprocessingParams();
							page_preprocessing_params->CorrectOrientation = VARIANT_TRUE;
							page_preprocessing_params->OrientationDetectionParams->put_OrientationDetectionMode(FREngine::OrientationDetectionModeEnum::ODM_Thorough);
							document->Preprocess(page_preprocessing_params, nullptr, nullptr, nullptr);

							if (document->Pages->Count < 1) {
								document->Close();
								continue;
							}

#ifdef LOG_USE_WOFSTREAM
							txt_file << L"-----------------------------------------------------" << std::endl;
							txt_file << L"File : " << files[i] << std::endl;
#endif

							const auto class_name = classify_document(engine, configuration, classification_engine, classification_model, files[i], document);
							if (class_name.empty()) {
								document->Close();
								continue;
							}

							const std::set<std::wstring> processed_class_names{
								L"Agreement Cover Page",
								L"Financial Transaction Information Request",
								L"Debt Repayment Inquiry",
								L"Bottom Contact Info Page",
								L"FTIR Next Page With Contact Info",
								L"Confiscation Search Verification Warrent",
								L"Certificate Of Trial",
								L"IDCard Of A PO With Contact Info",
								L"FTIR Next Page With Email",
								L"Cover Page Top Down Info",
								// L"Cover Page Tax Service Fax"
								// L"Cover Page Tax Service Logo",
								// L"Cover Page Police Agency Logo",
							};

							// 인식할 필요가 없는 문서는 pass
							if (processed_class_names.find(class_name) == processed_class_names.end()) {
								document->Close();
								continue;
							}

							// 세트에 동의서가 포함되었는지 여부
							if (class_name == L"Agreement Cover Page") {
								has_agreement = true;

								document->Close();
								continue;
							}

							if (class_name == L"Certificate Of Trial") {
								has_trial_cert = true;

								document->Close();
								continue;
							}

							if (class_name == L"Debt Repayment Inquiry")
								is_dri_set = true;

							std::vector<block> blocks;
							cv::Size image_size;
							std::tie(blocks, std::ignore, image_size) = recognize_document(engine, configuration, class_name, files[i], document);
							if (image_size.area() == 0)
								image_size = estimate_paper_size(blocks);
							document->Close();

							const auto keywords = get_keywords(configuration, keywords_, class_name);
							const auto searched_fields = search_fields(class_name, keywords, blocks);

							std::unordered_map<std::wstring, std::wstring> extracted_fields;

							if (class_name == L"Financial Transaction Information Request") {
								const auto request_organ = extract_request_organ(configuration, class_name, searched_fields, blocks, image_size);
								const auto document_number = extract_document_number(class_name, searched_fields, blocks, image_size);
								const auto grace_period = extract_grace_period(class_name, searched_fields, blocks, image_size);
								const auto warrant_number_value = extract_warrant_number(class_name, searched_fields, blocks, image_size);
								const auto warrant_number = std::get<1>(warrant_number_value);
								const auto use_purpose = extract_use_purpose(configuration, class_name, searched_fields, blocks, image_size);
								const auto telephone_number = extract_telephone_number(class_name, searched_fields, blocks, image_size, request_organ);
								const auto fax_number = extract_fax_number(class_name, searched_fields, blocks, telephone_number, image_size);
								const auto email_address = extract_email_address(class_name, searched_fields, blocks, image_size);
								auto manager_position_value = extract_manager_position(configuration, class_name, searched_fields, blocks, image_size, request_organ);;
								auto manager_position = std::get<1>(manager_position_value);
								const auto executive_name_value = extract_executive_name(class_name, searched_fields, blocks, image_size, request_organ);
								auto executive_name = std::get<1>(executive_name_value);
								const auto manager_name = extract_manager_name(class_name, searched_fields, blocks, manager_position_value, executive_name_value, image_size, request_organ);

								fill_field(extracted_fields, L"DEMAND_INSTI", request_organ);
								fill_field(extracted_fields, L"DOC_NO", document_number);
								fill_field(extracted_fields, L"NOTICE_DTRRD_PRD", grace_period);
								fill_field(extracted_fields, L"USE_OBJCTV_CD", use_purpose);
								fill_field(extracted_fields, L"WARRNT_DOC_NO", warrant_number);
								fill_field(extracted_fields, L"TELNO", telephone_number);
								fill_field(extracted_fields, L"FAX_NO", fax_number);
								fill_field(extracted_fields, L"EMAIL_ID", email_address);
								fill_field(extracted_fields, L"DMND_INCHRG_TLE", manager_position);
								fill_field(extracted_fields, L"DMND_INCHRG_NM", manager_name);
								fill_field(extracted_fields, L"DMND_CHRGP_NM", executive_name);
							}
							else if (class_name == L"Debt Repayment Inquiry") {
								const auto request_organ = extract_request_organ(configuration, class_name, searched_fields, blocks, image_size);
								const auto document_number = extract_document_number(class_name, searched_fields, blocks, image_size);
								const auto grace_period = extract_grace_period(class_name, searched_fields, blocks, image_size);
								const auto warrant_number_value = extract_warrant_number(class_name, searched_fields, blocks, image_size);
								const auto warrant_number = std::get<1>(warrant_number_value);
								const auto use_purpose = extract_use_purpose(configuration, class_name, searched_fields, blocks, image_size);
								const auto telephone_number = extract_telephone_number(class_name, searched_fields, blocks, image_size, request_organ);
								const auto fax_number = extract_fax_number(class_name, searched_fields, blocks, telephone_number, image_size);
								const auto email_address = extract_email_address(class_name, searched_fields, blocks, image_size);
								auto manager_position_value = extract_manager_position(configuration, class_name, searched_fields, blocks, image_size, request_organ);
								auto manager_position = std::get<1>(manager_position_value);
								const auto executive_name_value = extract_executive_name(class_name, searched_fields, blocks, image_size, request_organ);
								auto executive_name = std::get<1>(executive_name_value);
								const auto manager_name = extract_manager_name(class_name, searched_fields, blocks, manager_position_value, executive_name_value, image_size, request_organ);

								if (!manager_name.empty() && executive_name.empty()) {
									executive_name = manager_name;
								}

								fill_field(extracted_fields, L"DEMAND_INSTI", request_organ);
								fill_field(extracted_fields, L"DOC_NO", document_number);
								fill_field(extracted_fields, L"NOTICE_DTRRD_PRD", grace_period);
								fill_field(extracted_fields, L"USE_OBJCTV_CD", use_purpose);
								fill_field(extracted_fields, L"WARRNT_DOC_NO", warrant_number);
								fill_field(extracted_fields, L"TELNO", telephone_number);
								fill_field(extracted_fields, L"FAX_NO", fax_number);
								fill_field(extracted_fields, L"EMAIL_ID", email_address);
								fill_field(extracted_fields, L"DMND_INCHRG_TLE", manager_position);
								fill_field(extracted_fields, L"DMND_INCHRG_NM", manager_name);
								fill_field(extracted_fields, L"DMND_INCHRG_NM", executive_name);
							}
							else if (class_name == L"Confiscation Search Verification Warrent") {
								const auto warrant_number_value = extract_warrant_number(class_name, searched_fields, blocks, image_size);
								const auto warrant_number = std::get<1>(warrant_number_value);
								const auto crime_name = extract_crime_name(configuration, class_name, searched_fields, blocks, warrant_number_value, image_size);

								fill_field(extracted_fields, L"WARRNT_DOC_NO", warrant_number);
								fill_field(extracted_fields, L"CRIME_NM", crime_name);
							}
							else if (class_name == L"Bottom Contact Info Page") {
								const auto telephone_number = extract_telephone_number(class_name, searched_fields, blocks, image_size, L"");
								const auto fax_number = extract_fax_number(class_name, searched_fields, blocks, telephone_number, image_size);
								const auto email_address = extract_email_address(class_name, searched_fields, blocks, image_size);

								fill_field(extracted_fields, L"TELNO", telephone_number);
								fill_field(extracted_fields, L"FAX_NO", fax_number);
								fill_field(extracted_fields, L"EMAIL_ID", email_address);
							}
							else if (class_name == L"FTIR Next Page With Contact Info") {
								const auto telephone_number = extract_telephone_number(class_name, searched_fields, blocks, image_size, L"");
								const auto fax_number = extract_fax_number(class_name, searched_fields, blocks, telephone_number, image_size);
								const auto email_address = extract_email_address(class_name, searched_fields, blocks, image_size);
								const auto manager_name = extract_manager_name(class_name, searched_fields, blocks, std::make_pair(cv::Rect(), L""), std::make_pair(cv::Rect(), L""), image_size, L"");

								fill_field(extracted_fields, L"TELNO", telephone_number);
								fill_field(extracted_fields, L"FAX_NO", fax_number);
								fill_field(extracted_fields, L"EMAIL_ID", email_address);
								fill_field(extracted_fields, L"DMND_INCHRG_NM", manager_name);
							}
							else if (class_name == L"IDCard Of A PO With Contact Info") {
								const auto request_organ = extract_request_organ(configuration, class_name, searched_fields, blocks, image_size);
								const auto telephone_number = extract_telephone_number(class_name, searched_fields, blocks, image_size, L"");
								const auto fax_number = extract_fax_number(class_name, searched_fields, blocks, telephone_number, image_size);
								const auto email_address = extract_email_address(class_name, searched_fields, blocks, image_size);
								auto manager_position_value = extract_manager_position(configuration, class_name, searched_fields, blocks, image_size, request_organ);
								auto manager_position = std::get<1>(manager_position_value);
								const auto manager_name = extract_manager_name(class_name, searched_fields, blocks, manager_position_value, std::make_pair(cv::Rect(), L""), image_size, request_organ);

								fill_field(extracted_fields, L"DEMAND_INSTI", request_organ);
								fill_field(extracted_fields, L"TELNO", telephone_number);
								fill_field(extracted_fields, L"FAX_NO", fax_number);
								fill_field(extracted_fields, L"EMAIL_ID", email_address);
								fill_field(extracted_fields, L"DMND_INCHRG_TLE", manager_position);
								fill_field(extracted_fields, L"DMND_INCHRG_NM", manager_name);
							}
							else if (class_name == L"FTIR Next Page With Email") {
								const auto email_address = extract_email_address(class_name, searched_fields, blocks, image_size);

								fill_field(extracted_fields, L"EMAIL_ID", email_address);
							}
							else if (class_name == L"Cover Page Top Down Info") {
								const auto request_organ = extract_request_organ(configuration, class_name, searched_fields, blocks, image_size);
								const auto telephone_number = extract_telephone_number(class_name, searched_fields, blocks, image_size, L"");
								const auto fax_number = extract_fax_number(class_name, searched_fields, blocks, telephone_number, image_size);
								const auto email_address = extract_email_address(class_name, searched_fields, blocks, image_size);

								fill_field(extracted_fields, L"DEMAND_INSTI", request_organ);
								fill_field(extracted_fields, L"TELNO", telephone_number);
								fill_field(extracted_fields, L"FAX_NO", fax_number);
								fill_field(extracted_fields, L"EMAIL_ID", email_address);
							}

#if defined(_DEBUG)
							const auto image = debug_;
#endif

#ifdef LOG_USE_WOFSTREAM            // 문서단위 결과
							const auto text = to_wstring(blocks);
							txt_file << L"ALL : " << std::endl;
							txt_file << text << std::endl << std::endl;

							txt_file << L"- 파일 : " << files[i] << std::endl;
							txt_file << L"- 분류 : " << class_name << std::endl;

							txt_file << "- " << convert_field_column_to_string(L"DEMAND_INSTI") << " : " << get_field_value(extracted_fields, L"DEMAND_INSTI") << std::endl;
							txt_file << "- " << convert_field_column_to_string(L"DOC_NO") << " : " << get_field_value(extracted_fields, L"DOC_NO") << std::endl;
							txt_file << "- " << convert_field_column_to_string(L"NOTICE_DTRRD_PRD") << " : " << get_field_value(extracted_fields, L"NOTICE_DTRRD_PRD") << std::endl;
							txt_file << "- " << convert_field_column_to_string(L"USE_OBJCTV_CD") << " : " << get_field_value(extracted_fields, L"USE_OBJCTV_CD") << std::endl;
							txt_file << "- " << convert_field_column_to_string(L"WARRNT_DOC_NO") << " : " << get_field_value(extracted_fields, L"WARRNT_DOC_NO") << std::endl;
							txt_file << "- " << convert_field_column_to_string(L"CRIME_NM") << " : " << get_field_value(extracted_fields, L"CRIME_NM") << std::endl;
							txt_file << "- " << convert_field_column_to_string(L"TELNO") << " : " << get_field_value(extracted_fields, L"TELNO") << std::endl;
							txt_file << "- " << convert_field_column_to_string(L"FAX_NO") << " : " << get_field_value(extracted_fields, L"FAX_NO") << std::endl;
							txt_file << "- " << convert_field_column_to_string(L"EMAIL_ID") << " : " << get_field_value(extracted_fields, L"EMAIL_ID") << std::endl;
							txt_file << "- " << convert_field_column_to_string(L"DMND_INCHRG_TLE") << " : " << get_field_value(extracted_fields, L"DMND_INCHRG_TLE") << std::endl;
							txt_file << "- " << convert_field_column_to_string(L"DMND_INCHRG_NM") << " : " << get_field_value(extracted_fields, L"DMND_INCHRG_NM") << std::endl;
							txt_file << "- " << convert_field_column_to_string(L"DMND_CHRGP_NM") << " : " << get_field_value(extracted_fields, L"DMND_CHRGP_NM") << std::endl;
#endif

							if (extracted_fields.empty())
								continue;

							if (fields.find(class_name) == fields.end())
								fields.emplace(class_name, std::vector<std::unordered_map<std::wstring, std::wstring>>());

							fields.at(class_name).emplace_back(extracted_fields);
						}

						release_engine_object(std::make_pair(loader, engine));
					// } , std::stoi(configuration.at(L"engine").at(L"concurrency")));
				}
				catch (_com_error& e) {
					spdlog::get("recognizer")->error("exception : {} : ({} : {})", to_cp949(e.Description().GetBSTR()), __FILE__, __LINE__);
				}

				const std::unordered_map<std::wstring, std::wstring> db_default_value = {
					{ L"DEMAND_INSTI", DEFAULT_FAX_COLUMN_VALUE_FOR_STRING },
					{ L"INFO_REQ_INSTI_MK", DEFAULT_FAX_COLUMN_VALUE_FOR_CODE },
					{ L"DLV_AMT_MK", DEFAULT_FAX_COLUMN_VALUE_FOR_STRING },
					{ L"DOC_NO", DEFAULT_FAX_COLUMN_VALUE_FOR_STRING },
					{ L"WARRNT_DOC_NO", DEFAULT_FAX_COLUMN_VALUE_FOR_STRING },
					{ L"DMND_INCHRG_TLE", DEFAULT_FAX_COLUMN_VALUE_FOR_STRING },
					{ L"DMND_INCHRG_NM", DEFAULT_FAX_COLUMN_VALUE_FOR_STRING },
					{ L"DMND_CHRGP_NM", DEFAULT_FAX_COLUMN_VALUE_FOR_STRING },
					{ L"TELNO", DEFAULT_FAX_COLUMN_VALUE_FOR_STRING },
					{ L"FAX_NO", DEFAULT_FAX_COLUMN_VALUE_FOR_STRING },
					{ L"LEGAL_BASE_MK", DEFAULT_FAX_COLUMN_VALUE_FOR_CODE },
					{ L"USE_OBJCTV_CD", DEFAULT_FAX_COLUMN_VALUE_FOR_CODE },
					{ L"NOTICE_DTRRD_PRD", DEFAULT_FAX_COLUMN_VALUE_FOR_PRD },
					{ L"NOTICE_DTRRD_RSN", DEFAULT_FAX_COLUMN_VALUE_FOR_CODE },
					{ L"CRIME_NM", DEFAULT_FAX_COLUMN_VALUE_FOR_CODE },
					{ L"EMAIL_ID", DEFAULT_FAX_COLUMN_VALUE_FOR_STRING },
					{ L"DOC_TYPE", DEFAULT_FAX_COLUMN_VALUE_FOR_CODE },
					{ L"OTHER1", DEFAULT_FAX_COLUMN_VALUE_FOR_OTHER },
					{ L"OTHER2", DEFAULT_FAX_COLUMN_VALUE_FOR_OTHER },
					{ L"OTHER3", DEFAULT_FAX_COLUMN_VALUE_FOR_OTHER },
					{ L"OTHER4", DEFAULT_FAX_COLUMN_VALUE_FOR_OTHER },
					{ L"OTHER5", DEFAULT_FAX_COLUMN_VALUE_FOR_OTHER },
				};

				std::unordered_map<std::wstring, std::vector<std::wstring>> db_fields = {
					{ L"DEMAND_INSTI", { DEFAULT_FAX_COLUMN_VALUE_FOR_STRING } },
					{ L"INFO_REQ_INSTI_MK", { DEFAULT_FAX_COLUMN_VALUE_FOR_CODE } },
					{ L"DLV_AMT_MK", { DEFAULT_FAX_COLUMN_VALUE_FOR_CODE } },
					{ L"DOC_NO", { DEFAULT_FAX_COLUMN_VALUE_FOR_STRING } },
					{ L"WARRNT_DOC_NO", { DEFAULT_FAX_COLUMN_VALUE_FOR_STRING } },
					{ L"DMND_INCHRG_TLE", { DEFAULT_FAX_COLUMN_VALUE_FOR_STRING } },
					{ L"DMND_INCHRG_NM", { DEFAULT_FAX_COLUMN_VALUE_FOR_STRING } },
					{ L"DMND_CHRGP_NM", { DEFAULT_FAX_COLUMN_VALUE_FOR_STRING } },
					{ L"TELNO", { DEFAULT_FAX_COLUMN_VALUE_FOR_STRING } },
					{ L"FAX_NO", { DEFAULT_FAX_COLUMN_VALUE_FOR_STRING } },
					{ L"LEGAL_BASE_MK", { DEFAULT_FAX_COLUMN_VALUE_FOR_CODE } },
					{ L"USE_OBJCTV_CD", { DEFAULT_FAX_COLUMN_VALUE_FOR_CODE } },
					{ L"NOTICE_DTRRD_PRD", { DEFAULT_FAX_COLUMN_VALUE_FOR_PRD } },
					{ L"NOTICE_DTRRD_RSN", { DEFAULT_FAX_COLUMN_VALUE_FOR_CODE } },
					{ L"CRIME_NM", { DEFAULT_FAX_COLUMN_VALUE_FOR_CODE } },
					{ L"EMAIL_ID", { DEFAULT_FAX_COLUMN_VALUE_FOR_STRING } },
					{ L"DOC_TYPE", { DEFAULT_FAX_COLUMN_VALUE_FOR_CODE } },
					{ L"OTHER1", { DEFAULT_FAX_COLUMN_VALUE_FOR_OTHER } },
					{ L"OTHER2", { DEFAULT_FAX_COLUMN_VALUE_FOR_OTHER } },
					{ L"OTHER3", { DEFAULT_FAX_COLUMN_VALUE_FOR_OTHER } },
					{ L"OTHER4", { DEFAULT_FAX_COLUMN_VALUE_FOR_OTHER } },
					{ L"OTHER5", { DEFAULT_FAX_COLUMN_VALUE_FOR_OTHER } },
				};

				const std::vector<std::wstring> ordered_categories = {
					L"Financial Transaction Information Request",
					L"Debt Repayment Inquiry",
					L"Confiscation Search Verification Warrent",
					L"Bottom Contact Info Page",
					L"FTIR Next Page With Contact Info",
					L"IDCard Of A PO With Contact Info",
					L"FTIR Next Page With Email",
					L"Cover Page Top Down Info",
					// L"Cover Page Tax Service Fax",
					// L"Cover Page Tax Service Logo",
					// L"Cover Page Police Agency Logo",
				};

				// 인식된 값을 넣는다. 0번 Index 만 사용!!!
				for (const auto& category : ordered_categories) {
					if (fields.find(category) != fields.end() && !fields.at(category).empty()) {
						const auto field_map = fields.at(category)[0];

						for (const auto& field : field_map) {
							if (!std::get<1>(field).empty()
								&& db_fields.at(std::get<0>(field))[0] == db_default_value.at(std::get<0>(field))) {
								db_fields.at(std::get<0>(field))[0] = std::get<1>(field);
							}
						}
					}
				}

				// 통보유예기간의 경우 인식된 값이 없으면 default 0 을 사용
				/*if (db_fields.at(L"NOTICE_DTRRD_PRD")[0] == DEFAULT_FAX_COLUMN_VALUE)
				db_fields.at(L"NOTICE_DTRRD_PRD")[0] = L"0",*/

				// 담당자 직책과 이름이 같을 경우 오인식이므로 처리
				if (db_fields.at(L"DMND_INCHRG_TLE")[0] == db_fields.at(L"DMND_INCHRG_NM")[0])
					db_fields.at(L"DMND_INCHRG_TLE")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_STRING;

				const auto request_organ = db_fields.at(L"DEMAND_INSTI")[0];
				if (request_organ.find(L"국세청") != std::wstring::npos || request_organ.find(L"세무서") != std::wstring::npos) {
					if (!has_agreement) {
						db_fields.at(L"INFO_REQ_INSTI_MK")[0] = L"15";
						db_fields.at(L"DLV_AMT_MK")[0] = L"03";
						db_fields.at(L"LEGAL_BASE_MK")[0] = L"03";
						if (db_fields.at(L"USE_OBJCTV_CD")[0] == L"상속세" || db_fields.at(L"USE_OBJCTV_CD")[0] == L"증여세")
							db_fields.at(L"USE_OBJCTV_CD")[0] = L"3";
						else
							db_fields.at(L"USE_OBJCTV_CD")[0] = L"2";
						db_fields.at(L"CRIME_NM")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
						db_fields.at(L"NOTICE_DTRRD_RSN")[0] = L"2";
						db_fields.at(L"DOC_TYPE")[0] = L"1";
					}
					else {
						db_fields.at(L"INFO_REQ_INSTI_MK")[0] = L"01";
						db_fields.at(L"DLV_AMT_MK")[0] = L"05";
						db_fields.at(L"LEGAL_BASE_MK")[0] = L"01";
						db_fields.at(L"USE_OBJCTV_CD")[0] = L"2";
						db_fields.at(L"CRIME_NM")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
						db_fields.at(L"NOTICE_DTRRD_RSN")[0] = L"2";
						db_fields.at(L"DOC_TYPE")[0] = L"1";
					}
				}
				else if (request_organ.find(L"관세청") != std::wstring::npos || request_organ.find(L"세관") != std::wstring::npos) {
					db_fields.at(L"INFO_REQ_INSTI_MK")[0] = L"14";
					db_fields.at(L"DLV_AMT_MK")[0] = L"02";
					db_fields.at(L"LEGAL_BASE_MK")[0] = L"08";
					db_fields.at(L"USE_OBJCTV_CD")[0] = L"2";
					db_fields.at(L"CRIME_NM")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
					db_fields.at(L"NOTICE_DTRRD_RSN")[0] = L"2";
					db_fields.at(L"DOC_TYPE")[0] = L"1";
				}
				else if (request_organ.find(L"검찰청") != std::wstring::npos) {
					if (!has_agreement) {
						db_fields.at(L"INFO_REQ_INSTI_MK")[0] = L"02";
						db_fields.at(L"DLV_AMT_MK")[0] = L"04";
						db_fields.at(L"LEGAL_BASE_MK")[0] = L"02";
						db_fields.at(L"USE_OBJCTV_CD")[0] = L"1";
						if (!has_trial_cert)
							db_fields.at(L"CRIME_NM")[0] = convert_crime_name_to_code(db_fields.at(L"CRIME_NM")[0]);
						else
							db_fields.at(L"CRIME_NM")[0] = L"07";
						db_fields.at(L"NOTICE_DTRRD_RSN")[0] = L"1";
						db_fields.at(L"DOC_TYPE")[0] = L"1";
					}
					else {
						db_fields.at(L"INFO_REQ_INSTI_MK")[0] = L"01";
						db_fields.at(L"DLV_AMT_MK")[0] = L"05";
						db_fields.at(L"LEGAL_BASE_MK")[0] = L"01";
						db_fields.at(L"USE_OBJCTV_CD")[0] = L"1";
						db_fields.at(L"CRIME_NM")[0] = L"14";
						db_fields.at(L"NOTICE_DTRRD_RSN")[0] = L"1";
						db_fields.at(L"DOC_TYPE")[0] = L"1";
					}
				}
				else if (request_organ.find(L"경찰서") != std::wstring::npos) {
					if (!has_agreement) {
						db_fields.at(L"INFO_REQ_INSTI_MK")[0] = L"02";
						db_fields.at(L"DLV_AMT_MK")[0] = L"01";
						db_fields.at(L"LEGAL_BASE_MK")[0] = L"02";
						db_fields.at(L"USE_OBJCTV_CD")[0] = L"1";
						if (!has_trial_cert)
							db_fields.at(L"CRIME_NM")[0] = convert_crime_name_to_code(db_fields.at(L"CRIME_NM")[0]);
						else
							db_fields.at(L"CRIME_NM")[0] = L"07";
						db_fields.at(L"NOTICE_DTRRD_RSN")[0] = L"1";
						db_fields.at(L"DOC_TYPE")[0] = L"1";
					}
					else {
						db_fields.at(L"INFO_REQ_INSTI_MK")[0] = L"01";
						db_fields.at(L"DLV_AMT_MK")[0] = L"05";
						db_fields.at(L"LEGAL_BASE_MK")[0] = L"01";
						db_fields.at(L"USE_OBJCTV_CD")[0] = L"1";
						db_fields.at(L"CRIME_NM")[0] = L"14";
						db_fields.at(L"NOTICE_DTRRD_RSN")[0] = L"1";
						db_fields.at(L"DOC_TYPE")[0] = L"1";
					}
				}
				else if (request_organ.find(L"경찰청") != std::wstring::npos) {
					db_fields.at(L"INFO_REQ_INSTI_MK")[0] = L"02";
					db_fields.at(L"DLV_AMT_MK")[0] = L"01";
					db_fields.at(L"LEGAL_BASE_MK")[0] = L"02";
					db_fields.at(L"USE_OBJCTV_CD")[0] = L"1";
					db_fields.at(L"CRIME_NM")[0] = convert_crime_name_to_code(db_fields.at(L"CRIME_NM")[0]);
					db_fields.at(L"NOTICE_DTRRD_RSN")[0] = L"1";
					db_fields.at(L"DOC_TYPE")[0] = L"1";
				}
				else if (request_organ.find(L"금융감독원") != std::wstring::npos) {
					db_fields.at(L"INFO_REQ_INSTI_MK")[0] = L"04";
					db_fields.at(L"DLV_AMT_MK")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
					db_fields.at(L"LEGAL_BASE_MK")[0] = L"12";
					db_fields.at(L"USE_OBJCTV_CD")[0] = L"5";
					db_fields.at(L"CRIME_NM")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
					db_fields.at(L"NOTICE_DTRRD_RSN")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
					db_fields.at(L"DOC_TYPE")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
				}
				else if (request_organ.find(L"예금보험공사") != std::wstring::npos) {
					db_fields.at(L"INFO_REQ_INSTI_MK")[0] = L"05";
					db_fields.at(L"DLV_AMT_MK")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
					db_fields.at(L"LEGAL_BASE_MK")[0] = L"09";
					db_fields.at(L"USE_OBJCTV_CD")[0] = L"5";
					db_fields.at(L"CRIME_NM")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
					db_fields.at(L"NOTICE_DTRRD_RSN")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
					db_fields.at(L"DOC_TYPE")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
				}
				else if (request_organ.find(L"은행") != std::wstring::npos || request_organ.find(L"금고") != std::wstring::npos
					|| request_organ.find(L"뱅크") != std::wstring::npos || request_organ.find(L"에셋") != std::wstring::npos
					|| request_organ.find(L"증권") != std::wstring::npos || request_organ.find(L"조합") != std::wstring::npos
					|| request_organ.find(L"수협") != std::wstring::npos || request_organ.find(L"신협") != std::wstring::npos
					|| request_organ.find(L"투자") != std::wstring::npos || request_organ.find(L"우정사업본부") != std::wstring::npos
					|| request_organ.find(L"농협") != std::wstring::npos) {
					db_fields.at(L"INFO_REQ_INSTI_MK")[0] = L"99";
					db_fields.at(L"DLV_AMT_MK")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
					db_fields.at(L"LEGAL_BASE_MK")[0] = L"11";
					db_fields.at(L"USE_OBJCTV_CD")[0] = L"5";
					db_fields.at(L"CRIME_NM")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
					db_fields.at(L"NOTICE_DTRRD_RSN")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
					db_fields.at(L"DOC_TYPE")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
				}
				else if (request_organ.find(L"보훈청") != std::wstring::npos || request_organ.find(L"보훈지청") != std::wstring::npos
					|| request_organ.find(L"병무청") != std::wstring::npos || request_organ.find(L"병무지청") != std::wstring::npos) {
					if (!has_agreement) {// ???
						db_fields.at(L"INFO_REQ_INSTI_MK")[0] = L"03";
						db_fields.at(L"DLV_AMT_MK")[0] = L"05";
						db_fields.at(L"LEGAL_BASE_MK")[0] = L"07";
						db_fields.at(L"USE_OBJCTV_CD")[0] = L"5";
						db_fields.at(L"CRIME_NM")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
						db_fields.at(L"NOTICE_DTRRD_RSN")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
						db_fields.at(L"DOC_TYPE")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
					}
					else {
						db_fields.at(L"INFO_REQ_INSTI_MK")[0] = L"01";
						db_fields.at(L"DLV_AMT_MK")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
						db_fields.at(L"LEGAL_BASE_MK")[0] = L"01";
						db_fields.at(L"USE_OBJCTV_CD")[0] = L"5";
						db_fields.at(L"CRIME_NM")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
						db_fields.at(L"NOTICE_DTRRD_RSN")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
						db_fields.at(L"DOC_TYPE")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
					}
				}
				else if (request_organ.find(L"감사원") != std::wstring::npos) {
					if (!has_agreement) {
						db_fields.at(L"INFO_REQ_INSTI_MK")[0] = L"06";
						db_fields.at(L"DLV_AMT_MK")[0] = L"05";
						db_fields.at(L"LEGAL_BASE_MK")[0] = L"13";
						db_fields.at(L"USE_OBJCTV_CD")[0] = L"5";
						db_fields.at(L"CRIME_NM")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
						db_fields.at(L"NOTICE_DTRRD_RSN")[0] = L"1";
						db_fields.at(L"DOC_TYPE")[0] = L"1";
					}
					else {
						db_fields.at(L"iNFO_REQ_INSTI_MK")[0] = L"01";
						db_fields.at(L"DLV_AMT_MK")[0] = L"05";
						db_fields.at(L"LEGAL_BASE_MK")[0] = L"01";
						db_fields.at(L"USE_OBJCTV_CD")[0] = L"5";
						db_fields.at(L"CRIME_NM")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
						db_fields.at(L"NOTICE_DTRRD_RSN")[0] = L"1";
						db_fields.at(L"DOC_TYPE")[0] = L"1";
					}
				}
				else if (request_organ.find_last_of(L"시") != std::wstring::npos && request_organ.find_last_of(L"시") == request_organ.size() - 1) {
					db_fields.at(L"INFO_REQ_INSTI_MK")[0] = L"03";
					db_fields.at(L"DLV_AMT_MK")[0] = L"05";
					db_fields.at(L"LEGAL_BASE_MK")[0] = L"07";
					db_fields.at(L"USE_OBJCTV_CD")[0] = L"2";
					db_fields.at(L"CRIME_NM")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
					db_fields.at(L"NOTICE_DTRRD_RSN")[0] = L"2";
					db_fields.at(L"DOC_TYPE")[0] = L"1";
				}
				else {
					db_fields.at(L"CRIME_NM")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
				}

				if (is_dri_set) {
					db_fields.at(L"INFO_REQ_INSTI_MK")[0] = L"15";
					db_fields.at(L"DLV_AMT_MK")[0] = L"03";
					db_fields.at(L"LEGAL_BASE_MK")[0] = L"03";
					db_fields.at(L"USE_OBJCTV_CD")[0] = L"2";
					db_fields.at(L"CRIME_NM")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
					db_fields.at(L"NOTICE_DTRRD_PRD")[0] = L"0";
					db_fields.at(L"NOTICE_DTRRD_RSN")[0] = DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
					db_fields.at(L"DOC_TYPE")[0] = L"1";

					if (db_fields.at(L"DMND_CHRGP_NM")[0] == DEFAULT_FAX_COLUMN_VALUE_FOR_STRING)
						db_fields.at(L"DMND_CHRGP_NM")[0] == db_fields.at(L"DMND_INCHRG_NM")[0];

					if (db_fields.at(L"DMND_INCHRG_TLE")[0] == DEFAULT_FAX_COLUMN_VALUE_FOR_STRING)
						db_fields.at(L"DMND_INCHRG_TLE")[0] = L"국세조사관";

				}

#ifdef LOG_USE_WOFSTREAM
				txt_file << L"=====================================================" << std::endl;

				txt_file << "= " << convert_field_column_to_string(L"DEMAND_INSTI") << " : " << db_fields.at(L"DEMAND_INSTI")[0] << std::endl;
				txt_file << "= " << convert_field_column_to_string(L"INFO_REQ_INSTI_MK") << " : " << convert_info_req_insti_mk_to_string(db_fields.at(L"INFO_REQ_INSTI_MK")[0]) << std::endl;
				txt_file << "= " << convert_field_column_to_string(L"DLV_AMT_MK") << " : " << convert_dlv_amt_mk_to_string(db_fields.at(L"DLV_AMT_MK")[0]) << std::endl;
				txt_file << "= " << convert_field_column_to_string(L"LEGAL_BASE_MK") << " : " << convert_legal_base_mk_to_string(db_fields.at(L"LEGAL_BASE_MK")[0]) << std::endl;
				txt_file << "= " << convert_field_column_to_string(L"USE_OBJCTV_CD") << " : " << convert_use_objctv_cd_to_string(db_fields.at(L"USE_OBJCTV_CD")[0]) << std::endl;
				txt_file << "= " << convert_field_column_to_string(L"CRIME_NM") << " : " << convert_crime_nm_to_string(db_fields.at(L"CRIME_NM")[0]) << std::endl;
				txt_file << "= " << convert_field_column_to_string(L"DOC_TYPE") << " : " << convert_notice_doc_type_to_string(db_fields.at(L"DOC_TYPE")[0]) << std::endl;
				txt_file << "= " << convert_field_column_to_string(L"NOTICE_DTRRD_RSN") << " : " << convert_notice_dtrrd_rsn_to_string(db_fields.at(L"NOTICE_DTRRD_RSN")[0]) << std::endl;

				txt_file << "= " << convert_field_column_to_string(L"DOC_NO") << " : " << db_fields.at(L"DOC_NO")[0] << std::endl;
				txt_file << "= " << convert_field_column_to_string(L"WARRNT_DOC_NO") << " : " << db_fields.at(L"WARRNT_DOC_NO")[0] << std::endl;
				txt_file << "= " << convert_field_column_to_string(L"NOTICE_DTRRD_PRD") << " : " << db_fields.at(L"NOTICE_DTRRD_PRD")[0] << std::endl;
				txt_file << "= " << convert_field_column_to_string(L"TELNO") << " : " << db_fields.at(L"TELNO")[0] << std::endl;
				txt_file << "= " << convert_field_column_to_string(L"FAX_NO") << " : " << db_fields.at(L"FAX_NO")[0] << std::endl;
				txt_file << "= " << convert_field_column_to_string(L"EMAIL_ID") << " : " << db_fields.at(L"EMAIL_ID")[0] << std::endl;
				txt_file << "= " << convert_field_column_to_string(L"DMND_INCHRG_TLE") << " : " << db_fields.at(L"DMND_INCHRG_TLE")[0] << std::endl;
				txt_file << "= " << convert_field_column_to_string(L"DMND_INCHRG_NM") << " : " << db_fields.at(L"DMND_INCHRG_NM")[0] << std::endl;
				txt_file << "= " << convert_field_column_to_string(L"DMND_CHRGP_NM") << " : " << db_fields.at(L"DMND_CHRGP_NM")[0] << std::endl;

				txt_file << "= " << convert_field_column_to_string(L"OTHER1") << " : " << db_fields.at(L"OTHER1")[0] << std::endl;
				txt_file << "= " << convert_field_column_to_string(L"OTHER2") << " : " << db_fields.at(L"OTHER2")[0] << std::endl;
				txt_file << "= " << convert_field_column_to_string(L"OTHER3") << " : " << db_fields.at(L"OTHER3")[0] << std::endl;
				txt_file << "= " << convert_field_column_to_string(L"OTHER4") << " : " << db_fields.at(L"OTHER4")[0] << std::endl;
				txt_file << "= " << convert_field_column_to_string(L"OTHER5") << " : " << db_fields.at(L"OTHER5")[0] << std::endl;

				txt_file << L"=====================================================" << std::endl;

				txt_file.close();
#endif

				return db_fields;
			}

		private:
#ifdef LOG_USE_WOFSTREAM
			static std::wstring
				convert_field_column_to_string(const std::wstring& field_column) {
				const std::unordered_map<std::wstring, std::wstring> field_column_name_map = {
					{ L"DEMAND_INSTI", L"요구기관" },
					{ L"INFO_REQ_INSTI_MK", L"정보요청기관구분" },
					{ L"DLV_AMT_MK", L"우송료청구구분" },
					{ L"DOC_NO", L"문서번호" },
					{ L"WARRNT_DOC_NO", L"영장번호" },
					{ L"DMND_INCHRG_TLE", L"담당자직책" },
					{ L"DMND_INCHRG_NM", L"담당자성명" },
					{ L"DMND_CHRGP_NM", L"책임자성명" },
					{ L"TELNO", L"전화번호" },
					{ L"FAX_NO", L"팩스번호" },
					{ L"LEGAL_BASE_MK", L"법적근거구분" },
					{ L"USE_OBJCTV_CD", L"사용목적코드" },
					{ L"NOTICE_DTRRD_PRD", L"통보유예기간" },
					{ L"NOTICE_DTRRD_RSN", L"통보유예사유" },
					{ L"CRIME_NM", L"범죄명" },
					{ L"EMAIL_ID", L"이메일" },
					{ L"DOC_TYPE", L"통보문서양식" },
					{ L"OTHER1", L"OTHER1" },
					{ L"OTHER2", L"OTHER2" },
					{ L"OTHER3", L"OTHER3" },
					{ L"OTHER4", L"OTHER4" },
					{ L"OTHER5", L"OTHER5" },
				};

				for (const auto& field_column_name : field_column_name_map) {
					if (field_column == std::get<0>(field_column_name))
						return std::get<1>(field_column_name);
				}

				return L"";
			}

			static std::wstring
				get_field_value(const std::unordered_map<std::wstring, std::wstring>& extracted_fields, const std::wstring& field_column) {
				for (const auto& field : extracted_fields) {
					if (std::get<0>(field) == field_column) {
						return std::get<1>(field);
					}
				}

				return L"";
			}

			static std::vector<std::wstring>
				get_field_values(const std::unordered_map<std::wstring, std::vector<std::wstring>>& extracted_fields, const std::wstring& field_column) {
				for (const auto& field : extracted_fields) {
					if (std::get<0>(field) == field_column) {
						return std::get<1>(field);
					}
				}

				return std::vector<std::wstring>();
			}



			static std::wstring
				convert_info_req_insti_mk_to_string(const std::wstring& field_column) {
				const std::unordered_map<std::wstring, std::wstring> info_req_insti_mk_map = {
					{ L"01", L"01. 본인의 동의" },
					{ L"02", L"02. 법원의 영장" },
					{ L"03", L"03. 지방자치단체" },
					{ L"04", L"04. 금융감독원" },
					{ L"05", L"05. 예금보험공사" },
					{ L"06", L"06. 감사원" },
					{ L"07", L"07. 중앙선거관리위원회" },
					{ L"08", L"08. 공직자윤리위원회" },
					{ L"09", L"09. 금융분석원" },
					{ L"10", L"10. 기획재정부" },
					{ L"11", L"11. 공정거래위원회" },
					{ L"12", L"12. 한국증권선물거래소" },
					{ L"13", L"13. 외국금융감독기관" },
					{ L"14", L"14. 관세청(세관)" },
					{ L"15", L"15. 국세청(세무서)" },
					{ L"16", L"16. 허가서" },
					{ L"99", L"99. 기타" },
				};

				for (const auto& info_req_insti_mk : info_req_insti_mk_map) {
					if (field_column == std::get<0>(info_req_insti_mk))
						return std::get<1>(info_req_insti_mk);
				}

				return DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
			}

			static std::wstring
				convert_dlv_amt_mk_to_string(const std::wstring& field_column) {
				const std::unordered_map<std::wstring, std::wstring> dlv_amt_mk_map = {
					{ L"01", L"01. 경찰(청,서)" },
					{ L"02", L"02. 관세청(세관)" },
					{ L"03", L"03. 국세청" },
					{ L"04", L"04. 대검찰청(검찰청)" },
					{ L"05", L"05. 기타(즉시입금)" },
				};

				for (const auto& dlv_amt_mk : dlv_amt_mk_map) {
					if (field_column == std::get<0>(dlv_amt_mk))
						return std::get<1>(dlv_amt_mk);
				}

				return DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
			}

			static std::wstring
				convert_legal_base_mk_to_string(const std::wstring& field_column) {
				const std::unordered_map<std::wstring, std::wstring> legal_base_mk_map = {
					{ L"01", L"01. 동의서" },
					{ L"02", L"02. 영장" },
					{ L"03", L"03. 국세청" },
					{ L"04", L"04. 법원" },
					{ L"05", L"05. 공직자" },
					{ L"06", L"06. 병무청" },
					{ L"07", L"07. 지방세" },
					{ L"08", L"08. 관세" },
					{ L"09", L"09. 예보" },
					{ L"10", L"10. 선거관리" },
					{ L"11", L"11. 협조문서" },
					{ L"12", L"12. 금융감독원" },
					{ L"13", L"13. 감사원" },
				};

				for (const auto& legal_base_mk : legal_base_mk_map) {
					if (field_column == std::get<0>(legal_base_mk))
						return std::get<1>(legal_base_mk);
				}

				return DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
			}

			static std::wstring
				convert_use_objctv_cd_to_string(const std::wstring& field_column) {
				const std::unordered_map<std::wstring, std::wstring> use_objctv_cd_map = {
					{ L"1", L"1. 수사상" },
					{ L"2", L"2. 조세관련" },
					{ L"3", L"3. 상속세 및 증여세" },
					{ L"4", L"4. 공직자재산조사" },
					{ L"5", L"5. 기타" },
				};

				for (const auto& use_objctv_cd : use_objctv_cd_map) {
					if (field_column == std::get<0>(use_objctv_cd))
						return std::get<1>(use_objctv_cd);
				}

				return DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
			}

			static std::wstring
				convert_notice_dtrrd_rsn_to_string(const std::wstring& field_column) {
				const std::unordered_map<std::wstring, std::wstring> notice_dtrrd_rsn_map = {
					{ L"1", L"1. 증거인멸" },
					{ L"2", L"2. 세무조사" },
					{ L"3", L"3. 제출명령" },
					{ L"4", L"4. 신변의 위협" },
				};

				for (const auto& notice_dtrrd_rsn : notice_dtrrd_rsn_map) {
					if (field_column == std::get<0>(notice_dtrrd_rsn))
						return std::get<1>(notice_dtrrd_rsn);
				}

				return DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
			}

			static std::wstring
				convert_crime_nm_to_string(const std::wstring& field_column) {
				const std::unordered_map<std::wstring, std::wstring> crime_nm_map = {
					{ L"01", L"01. 뇌물,알선수재(수뢰)" },
					{ L"02", L"횡령" },
					{ L"03", L"사기,사기자금" },
					{ L"04", L"정치자금법위반" },
					{ L"05", L"상습도박&복표" },
					{ L"06", L"06. 통화,유가증권위조" },
					{ L"07", L"07. 범죄수익" },
					{ L"08", L"08. 자금세탁" },
					{ L"09", L"09. 절도,징물,도난" },
					{ L"10", L"10. 조직범죄" },
					{ L"11", L"11. 증권거래법위반" },
					{ L"12", L"12. 밀수,마약범죄" },
					{ L"13", L"13. 테러자금조달" },
					{ L"14", L"14. 기타" },
				};

				for (const auto& crime_nm : crime_nm_map) {
					if (field_column == std::get<0>(crime_nm))
						return std::get<1>(crime_nm);
				}

				return DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
			}

			static std::wstring
				convert_notice_doc_type_to_string(const std::wstring& field_column) {
				const std::unordered_map<std::wstring, std::wstring> notice_doc_type_map = {
					{ L"1", L"1. 개인" },
				};

				for (const auto& notice_doc_type : notice_doc_type_map) {
					if (field_column == std::get<0>(notice_doc_type))
						return std::get<1>(notice_doc_type);
				}

				return DEFAULT_FAX_COLUMN_VALUE_FOR_CODE;
			}
#endif
			static std::pair<cv::Rect, std::wstring>
				preprocess_request_organ(const std::pair<cv::Rect, std::wstring>& request_organ)
			{
				std::wstring requester = boost::regex_replace(std::get<1>(request_organ), boost::wregex(L" "), L"");
				requester = boost::regex_replace(requester, boost::wregex(L"국[세|새]{1}[칭|청]{1}"), L"국세청");
				requester = boost::regex_replace(requester, boost::wregex(L"[새|세]{1}[무|문]{1}서"), L"세무서");
				requester = boost::regex_replace(requester, boost::wregex(L"업무자원센터"), L"업무지원센터");
				requester = boost::regex_replace(requester, boost::wregex(L"은행"), L"은행");
				requester = boost::regex_replace(requester, boost::wregex(L"김●사원"), L"감사원");

				return std::make_pair(std::get<0>(request_organ), requester);
			}

			static std::pair<cv::Rect, std::wstring>
				preprocess_email_address(const std::pair<cv::Rect, std::wstring>& email)
			{
				std::wstring email_address = boost::regex_replace(std::get<1>(email), boost::wregex(L"ⓒ"), L"ⓒ");
				email_address = boost::regex_replace(email_address, boost::wregex(L","), L".");
				email_address = boost::regex_replace(email_address, boost::wregex(L"go.kf"), L"go.kr");
				email_address = boost::regex_replace(email_address, boost::wregex(L"nls.go.kr"), L"nts.go.kr");

				return std::make_pair(std::get<0>(email), email_address);
			}


			
			

			static std::wstring
				postprocess_email_address(const std::wstring& email_address)
			{
				return boost::regex_replace(email_address, boost::wregex(L" "), L"");
			}

			static std::pair<cv::Rect, std::wstring>
				preprocess_telephone_number(const std::pair<cv::Rect, std::wstring>& telephone_number)
			{
				std::wstring number = boost::regex_replace(std::get<1>(telephone_number), boost::wregex(L" "), L"");
				number = boost::regex_replace(number, boost::wregex(L"\\."), L"");
				number = boost::regex_replace(number, boost::wregex(L"CX3"), L"00");
				number = boost::regex_replace(number, boost::wregex(L"l"), L"1");
				number = boost::regex_replace(number, boost::wregex(L"([팩백]{1}스|[fF]{1}ax)(.*)"), L"");

				return std::make_pair(std::get<0>(telephone_number), number);
			}

			static std::wstring
				postprocess_telephone_number(const std::wstring& telephone_number)
			{
				auto parsed_number = boost::regex_replace(telephone_number, boost::wregex(L"[^0-9]"), L"");
				std::wstring ret_number(L"");

				switch (parsed_number.size()) {
				case 7: // 123-4567
				case 8: // 1234-5678
					if (parsed_number[0] != L'0')
						ret_number = parsed_number.insert(parsed_number.size() - 4, L"-");
					break;
				case 9: // 02-123-4567
					if (parsed_number.find(L"02") == 0) {
						ret_number = parsed_number.insert(parsed_number.size() - 4, L"-");
						ret_number = ret_number.insert(ret_number.size() - 8, L"-");
					}
					break;
				case 10: // 02-1234-5678 // 031-123-5678
					if (parsed_number.find(L"02") == 0) { // 02-1234-5678
						ret_number = parsed_number.insert(parsed_number.size() - 4, L"-");
						ret_number = ret_number.insert(ret_number.size() - 9, L"-");
					}
					else if (parsed_number.find(L"031") == 0 || parsed_number.find(L"032") == 0 || parsed_number.find(L"033") == 0 // 031-123-5678
						|| parsed_number.find(L"041") == 0 || parsed_number.find(L"042") == 0 || parsed_number.find(L"043") == 0
						|| parsed_number.find(L"044") == 0 || parsed_number.find(L"051") == 0 || parsed_number.find(L"052") == 0
						|| parsed_number.find(L"053") == 0 || parsed_number.find(L"054") == 0 || parsed_number.find(L"055") == 0
						|| parsed_number.find(L"061") == 0 || parsed_number.find(L"062") == 0 || parsed_number.find(L"063") == 0
						|| parsed_number.find(L"064") == 0) {
						ret_number = parsed_number.insert(parsed_number.size() - 4, L"-");
						ret_number = ret_number.insert(ret_number.size() - 8, L"-");
					}
					break;
				case 11: // 031-1234-5678
					if (parsed_number.find(L"031") == 0 || parsed_number.find(L"032") == 0 || parsed_number.find(L"033") == 0 // 031-123-5678
						|| parsed_number.find(L"041") == 0 || parsed_number.find(L"042") == 0 || parsed_number.find(L"043") == 0
						|| parsed_number.find(L"044") == 0 || parsed_number.find(L"051") == 0 || parsed_number.find(L"052") == 0
						|| parsed_number.find(L"053") == 0 || parsed_number.find(L"054") == 0 || parsed_number.find(L"055") == 0
						|| parsed_number.find(L"061") == 0 || parsed_number.find(L"062") == 0 || parsed_number.find(L"063") == 0
						|| parsed_number.find(L"064") == 0) {
						ret_number = parsed_number.insert(parsed_number.size() - 4, L"-");
						ret_number = ret_number.insert(ret_number.size() - 9, L"-");
					}
					break;
				default:
					break;
				}

				return ret_number;
			}

			static std::pair<cv::Rect, std::wstring>
				preprocess_fax_number(const std::pair<cv::Rect, std::wstring>& fax_number)
			{
				std::wstring number = boost::regex_replace(std::get<1>(fax_number), boost::wregex(L" "), L"");
				number = boost::regex_replace(number, boost::wregex(L"\\."), L"");
				number = boost::regex_replace(number, boost::wregex(L"CX3"), L"00");
				number = boost::regex_replace(number, boost::wregex(L"B0"), L"050");
				number = boost::regex_replace(number, boost::wregex(L"0[sS]{1}0"), L"050");
				number = boost::regex_replace(number, boost::wregex(L"030"), L"050");
				number = boost::regex_replace(number, boost::wregex(L"l"), L"1");

				return std::make_pair(std::get<0>(fax_number), number);
			}

			static std::wstring
				postprocess_fax_number(const std::wstring& fax_number)
			{
				auto parsed_number = boost::regex_replace(fax_number, boost::wregex(L"[^0-9]"), L"");
				std::wstring ret_number(L"");

				switch (parsed_number.size()) {
				case 7: // 123-4567
				case 8: // 1234-5678
					if (parsed_number[0] != L'0')
						ret_number = parsed_number.insert(parsed_number.size() - 4, L"-");
					break;
				case 9: // 02-123-4567
					if (parsed_number.find(L"02") == 0) {
						ret_number = parsed_number.insert(parsed_number.size() - 4, L"-");
						ret_number = ret_number.insert(ret_number.size() - 8, L"-");
					}
					break;
				case 10: // 02-1234-5678 // 031-123-5678
					if (parsed_number.find(L"02") == 0) { // 02-1234-5678
						ret_number = parsed_number.insert(parsed_number.size() - 4, L"-");
						ret_number = ret_number.insert(ret_number.size() - 9, L"-");
					}
					else if (parsed_number.find(L"031") == 0 || parsed_number.find(L"032") == 0 || parsed_number.find(L"033") == 0 // 031-123-5678
						|| parsed_number.find(L"041") == 0 || parsed_number.find(L"042") == 0 || parsed_number.find(L"043") == 0
						|| parsed_number.find(L"044") == 0 || parsed_number.find(L"051") == 0 || parsed_number.find(L"052") == 0
						|| parsed_number.find(L"053") == 0 || parsed_number.find(L"054") == 0 || parsed_number.find(L"055") == 0
						|| parsed_number.find(L"061") == 0 || parsed_number.find(L"062") == 0 || parsed_number.find(L"063") == 0
						|| parsed_number.find(L"064") == 0) {
						ret_number = parsed_number.insert(parsed_number.size() - 4, L"-");
						ret_number = ret_number.insert(ret_number.size() - 8, L"-");
					}
					break;
				case 11: // 031-1234-5678 // 050-1234-5678
					if (parsed_number.find(L"031") == 0 || parsed_number.find(L"032") == 0 || parsed_number.find(L"033") == 0 // 031-123-5678
						|| parsed_number.find(L"041") == 0 || parsed_number.find(L"042") == 0 || parsed_number.find(L"043") == 0
						|| parsed_number.find(L"044") == 0 || parsed_number.find(L"051") == 0 || parsed_number.find(L"052") == 0
						|| parsed_number.find(L"053") == 0 || parsed_number.find(L"054") == 0 || parsed_number.find(L"055") == 0
						|| parsed_number.find(L"061") == 0 || parsed_number.find(L"062") == 0 || parsed_number.find(L"063") == 0
						|| parsed_number.find(L"064") == 0) {
						ret_number = parsed_number.insert(parsed_number.size() - 4, L"-");
						ret_number = ret_number.insert(ret_number.size() - 9, L"-");
					}
					else if (parsed_number.find(L"050") == 0) { // 050-1234-5678 ==> // 0501-234-5678
						ret_number = parsed_number.insert(parsed_number.size() - 4, L"-");
						ret_number = ret_number.insert(ret_number.size() - 8, L"-");
					}
					break;
				default:
					break;
				}

				return ret_number;
			}

			static std::pair<cv::Rect, std::wstring>
				preprocess_position(const std::pair<cv::Rect, std::wstring>& position)
			{
				return std::make_pair(std::get<0>(position), boost::regex_replace(std::get<1>(position), boost::wregex(L"\\0"), L""));
			}

			static std::wstring
				postprocess_position(const std::wstring& position)
			{
				if (position.find(L"3급") != std::wstring::npos ||
					position.find(L"4급") != std::wstring::npos ||
					position.find(L"5급") != std::wstring::npos ||
					position.find(L"6급") != std::wstring::npos ||
					position.find(L"7급") != std::wstring::npos ||
					position.find(L"8급") != std::wstring::npos ||
					position.find(L"9급") != std::wstring::npos)

					return position;

				auto cleaned = boost::regex_replace(position, boost::wregex(L"(?:이?메일(?:주소)?|전화(?:번호)?|(?:e[-_]?)?mail(?:주소)?)?[^가-힣,]"), L"");
				cleaned = boost::regex_replace(cleaned, boost::wregex(L"(성명)(.*)"), L"");

				if (cleaned.size() < 2)
					return L"";

				return boost::regex_replace(cleaned, boost::wregex(L" "), L"");
			}

			static std::pair<cv::Rect, std::wstring>
				preprocess_name(const std::pair<cv::Rect, std::wstring>& name)
			{
				return std::make_pair(std::get<0>(name), boost::regex_replace(std::get<1>(name), boost::wregex(L"\\."), L","));
			}

			static std::wstring
				postprocess_name(const std::wstring& name)
			{
				auto cleaned = boost::regex_replace(name, boost::wregex(L"(?:이?메일(?:주소)?|전화(?:번호)?|팩스(?:번호)?|연락처?|(?:e[-_]?)?mail(?:주소)?)?[^가-힣,]"), L"");
				cleaned = boost::regex_replace(cleaned, boost::wregex(L"검사|검사보|경위|계장|과장|대리|부장|사무관|조사관|차장|주임|경감|경사|경장|경정|경제팀|수사관|순경|검찰수사관|계상보|계장보|고객행복센터|과장님|과장대리|과장보|담당자|부지점장|사이버팀|세무조사관|업무직|업무팀|주암|주임|직원|텔러|행원|검사역"), L"");
				cleaned = boost::regex_replace(name, boost::wregex(L"(?:이?메일(?:주소)|[전진견]{1}화(?:[번민빈인]{1}호)?|연락처|(?:e[-_]?)?mail(?:주소)|주소)"), L"");
				cleaned = boost::regex_replace(name, boost::wregex(L",,"), L"");
				cleaned = boost::regex_replace(name, boost::wregex(L"([전진견]{1}화[번민빈인]{1}호)"), L"");

				if (cleaned.size() < 2)
					return L"";

				return boost::regex_replace(cleaned, boost::wregex(L" "), L"");
			}

			static std::wstring
				postprocess_document_number(const std::wstring& document_number)
			{
				auto cleaned = boost::replace_all_copy(document_number, L"\n", L" ");
				cleaned = boost::regex_replace(cleaned, boost::wregex(L" "), L"");
				cleaned = boost::regex_replace(cleaned, boost::wregex(L"(20[0-9]{2}[.,]{1,2}[0-9]{1,2}[.,]{1,2}[0-9]{1,2}[,.]?)"), L"");
				cleaned = boost::regex_replace(cleaned, boost::wregex(L"(요구일자|담당자|전화|fax|금융거래|통보유예|특이사항|법적근거|사유|아닌|경우|상이)(.*)"), L"");
				cleaned = boost::regex_replace(cleaned, boost::wregex(L"[^-가-힣A-Za-z0-9]"), L"");
				cleaned = boost::regex_replace(cleaned, boost::wregex(L"[님|남]{1}세"), L"납세");

				if (cleaned.size() < 4 || cleaned.size() > 26)
					return L"";

				return cleaned;
			}

			static std::wstring
				postprocess_warrant_number(const std::wstring& warrant_number)
			{
				auto cleaned = boost::regex_replace(warrant_number, boost::wregex(L" "), L"");
				if (cleaned.size() >= 5 && cleaned.size() < 12 && cleaned.find(L"-") == std::wstring::npos) {
					cleaned = cleaned.insert(4, L"-");
				}

				return cleaned;
			}

			static std::pair<cv::Rect, std::wstring>
				preprocess_crime_name(const std::pair<cv::Rect, std::wstring>& crime_name)
			{
				return std::make_pair(std::get<0>(crime_name), boost::regex_replace(std::get<1>(crime_name), boost::wregex(L"[^가-힣]"), L""));
			}

			static std::wstring
				extract_request_organ(const configuration& configuration, const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>&
				fields,
				const std::vector<block>& blocks, const cv::Size& image_size)
			{
				if ((category != L"Debt Repayment Inquiry" && category != L"Cover Page Top Down Info")
					&& fields.find(L"요구기관명") == fields.end())
					return L"";

				if (category == L"Financial Transaction Information Request") {
					auto requested_organs = extract_field_values(fields.at(L"요구기관명"), blocks,
						std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						preprocess_request_organ,
						create_extract_function(configuration, L"requester", 2),
						postprocess_uppercase);

					// 요구기관명 값이 2~3줄인 경우
					if (requested_organs.empty()) {
						requested_organs = extract_field_values(fields.at(L"요구기관명"), blocks,
							std::bind(find_right_lines, std::placeholders::_1, std::placeholders::_2, 0.1, 1.6, 100, true),
							preprocess_request_organ,
							create_extract_function(configuration, L"requester", 2),
							postprocess_uppercase);
					}

					if (requested_organs.empty() && !blocks.empty()) {
						const auto paper = image_size;

						std::vector<line> bottom_lines;
						std::copy_if(std::begin(blocks[0]), std::end(blocks[0]), std::back_inserter(bottom_lines), [&](const line& line) {
							const auto line_rect = to_rect(line);
							return line_rect.y > paper.height * 3 / 4 && line.size() < 7 && line_rect.height > 27;
						});

						requested_organs = extract_field_values(std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>{
							std::make_tuple(cv::Rect(), L"", cv::Range())
						}, std::vector<block> {bottom_lines},
						default_search,
						preprocess_request_organ,
						create_extract_function(configuration, L"requester", 2),
						postprocess_uppercase);
					}

					if (requested_organs.empty() && fields.find(L"기관명") != fields.end()) {
						requested_organs = extract_field_values(fields.at(L"기관명"), blocks,
							std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
							preprocess_request_organ,
							create_extract_function(configuration, L"requester", 2),
							postprocess_uppercase);

						if (requested_organs.empty()) {
							requested_organs = extract_field_values(fields.at(L"기관명"), blocks,
								std::bind(find_right_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
								preprocess_request_organ,
								create_extract_function(configuration, L"requester", 2),
								postprocess_uppercase);
						}
					}

					if (requested_organs.empty()) {
						requested_organs = extract_field_values(fields.at(L"요구기관명"), blocks,
							search_self,
							preprocess_request_organ,
							create_extract_function(configuration, L"requester", 2),
							postprocess_uppercase);
					}

					return requested_organs.empty() ? L"" : std::get<1>(requested_organs[0]);
				}
				else if (category == L"Debt Repayment Inquiry") {
					auto dictionary = get_dictionary_words(configuration, dictionaries_, L"requester");

					boost::remove_erase_if(dictionary, [](const std::wstring& word) {
						return word.find(L"국세청") == std::wstring::npos && word.find(L"세무서") == std::wstring::npos;
					});

					aho_corasick::wtrie trie;
					trie.case_insensitive().remove_overlaps().allow_space();

					build_trie(trie, dictionary);
					const auto spell_dictionary = build_spell_dictionary(dictionary);

					auto text = to_wstring(blocks);

					std::vector<std::wstring> words;
					boost::algorithm::split(words, text, boost::is_any_of(L"\n\t "), boost::token_compress_on);

					for (auto& word : words) {
						const auto suggested = spell_dictionary->Correct(word);

						if (!suggested.empty() && suggested[0].distance <= 2)
							word = suggested[0].term;
					}

					text = boost::algorithm::join(words, L" ");

					text = boost::regex_replace(text, boost::wregex(L"[세새] ?[우무부] ?서?"), L"세무서");

					auto matches = trie.parse_text(text);
					boost::remove_erase_if(matches, [](const aho_corasick::wtrie::emit_type& token) {
						return token.get_keyword().size() == 3 || (token.get_keyword().find(L"세무서") == std::wstring::npos
							&& token.get_keyword().find(L"국세청") == std::wstring::npos);
					});

					std::sort(std::begin(matches), std::end(matches),
						[](const aho_corasick::wtrie::emit_type& a, const aho_corasick::wtrie::emit_type& b) {
						return a.get_keyword().size() > b.get_keyword().size();
					});

					if (matches.size() > 1)
						return matches.empty() ? L"" : matches.back().get_keyword();

					return matches.empty() ? L"" : matches.front().get_keyword();
				}
				else if (category == L"IDCard Of A PO With Contact Info") {
					auto requested_organs = extract_field_values(fields.at(L"요구기관명"), blocks,
						std::bind(find_right_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						preprocess_request_organ,
						create_extract_function(configuration, L"requester", 2),
						postprocess_uppercase);

					if (requested_organs.empty()) {
						requested_organs = extract_field_values(fields.at(L"요구기관명"), blocks,
							search_self,
							default_preprocess,
							create_extract_function(configuration, L"requester", 2),
							postprocess_uppercase);
					}

					if (requested_organs.empty()) {
						requested_organs = extract_field_values(std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>{
							std::make_tuple(cv::Rect(), L"", cv::Range())
						}, std::vector<block> {blocks[0]},
						default_search,
						preprocess_request_organ,
						create_extract_function(configuration, L"requester", 1),
						postprocess_uppercase);
					}

					if (requested_organs.empty())
						return L"";

					int max_height_idx = 0;
					for (int i = 0; i < requested_organs.size(); i++) {
						auto line_rect = to_rect(requested_organs[i]);
						max_height_idx = line_rect.height > max_height_idx ? i : max_height_idx;
					}

					return std::get<1>(requested_organs[max_height_idx]);
				}
				else if (category == L"Cover Page Top Down Info") {
					std::vector<line> top_lines;
					std::copy_if(std::begin(blocks[0]), std::end(blocks[0]), std::back_inserter(top_lines), [&](const line& line) {
						const auto line_rect = to_rect(line);
						return line_rect.y < image_size.height * 0.18;
					});

					auto requested_organs = extract_field_values(std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>{
						std::make_tuple(cv::Rect(), L"", cv::Range())
					}, std::vector<block> {top_lines},
					default_search,
					preprocess_request_organ,
					create_extract_function(configuration, L"requester", 1),
					postprocess_uppercase);

					return requested_organs.empty() ? L"" : std::get<1>(requested_organs[0]);
				}

				return L"";
			}

			static std::wstring
				extract_telephone_number(const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>&
				fields,
				const std::vector<block>& blocks, const cv::Size& image_size, const std::wstring request_organ)
			{
				if (fields.find(L"전화번호") == fields.end())
					return L"";

				std::vector<std::pair<cv::Rect, std::wstring>> telephone_numbers;

				if (category == L"Financial Transaction Information Request"
					|| category == L"Debt Repayment Inquiry"
					|| category == L"Bottom Contact Info Page"
					|| category == L"FTIR Next Page With Contact Info"
					|| category == L"IDCard Of A PO With Contact Info") {
					std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>> correct_fields;
					for (const auto& number_field : fields.at(L"전화번호")) {
						const auto number = std::get<1>(number_field);

						if (category == L"Financial Transaction Information Request" && request_organ == L"감사원" && (number.find(L"연락처") != std::wstring::npos || number.find(L"감사원") != std::wstring::npos)) {
							continue;
						}

						if (number.find(L"수신") != std::wstring::npos || number.find(L"대표") != std::wstring::npos) {
							continue;
						}

						bool has_incorrect_word = false;
						auto left_lines = find_left_lines(number_field, blocks, 0.5, 0.0, 100);
						for (const auto& line : left_lines) {
							const auto str = to_wstring(line);
							if (str.find(L"수신") != std::wstring::npos) {
								has_incorrect_word = true;
							}
						}

						if (!has_incorrect_word)
							correct_fields.push_back(number_field);
					}

					std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>> number_fields = { std::make_pair(L"전화번호", correct_fields) };

					telephone_numbers = extract_field_values(number_fields.at(L"전화번호"), blocks,
						std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						preprocess_telephone_number,
						create_extract_function(L"(((?:0(?:2|3[1-3]|4[1-4]|5[1-5]|6[1-4]|70))[)-]{0,2})?([0-9-]{7,10}))"),
						postprocess_telephone_number);

					if (telephone_numbers.empty()) {
						telephone_numbers = extract_field_values(number_fields.at(L"전화번호"), blocks,
							search_self,
							preprocess_telephone_number,
							create_extract_function(L"(((?:0(?:2|3[1-3]|4[1-4]|5[1-5]|6[1-4]|70))[)-]{0,2})?([0-9-]{7,10}))"),
							postprocess_telephone_number);
					}
				}
				else if (category == L"Cover Page Top Down Info") {
					std::vector<line> bottom_lines;
					std::copy_if(std::begin(blocks[0]), std::end(blocks[0]), std::back_inserter(bottom_lines), [&](const line& line) {
						const auto line_rect = to_rect(line);
						return line_rect.y > image_size.height * 0.77;
					});

					telephone_numbers = extract_field_values(fields.at(L"전화번호"), std::vector<block> {bottom_lines},
						std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						preprocess_telephone_number,
						create_extract_function(L"(((?:0(?:2|3[1-3]|4[1-4]|5[1-5]|6[1-4]|70))[)-]{0,2})?([0-9-]{7,10}))"),
						postprocess_telephone_number);

					if (telephone_numbers.empty()) {
						telephone_numbers = extract_field_values(fields.at(L"전화번호"), std::vector<block> {bottom_lines},
							search_self,
							preprocess_telephone_number,
							create_extract_function(L"(((?:0(?:2|3[1-3]|4[1-4]|5[1-5]|6[1-4]|70))[)-]{0,2})?([0-9-]{7,10}))"),
							postprocess_telephone_number);
					}
				}

				if (telephone_numbers.empty())
					return L"";

				boost::remove_erase_if(telephone_numbers, [&blocks](const std::pair<cv::Rect, std::wstring>& telephone_number) {
					const auto& left_line = find_nearest_left_line(std::make_tuple(std::get<0>(telephone_number), std::get<1>(telephone_number), cv::Range()), blocks, 0.5, 0.0, 100);
					if (!left_line.empty() && boost::regex_replace(std::get<1>(left_line[0]), boost::wregex(L"담당자 ?전화번호"), L"담당자전화번호") == L"담당자전화번호")
						return false;

					return std::get<0>(telephone_number).y < 120;
				});

				if (fields.find(L"책임자") != fields.end()) {
					for (const auto& executive_field : fields.at(L"책임자")) {
						const auto executive_telephone_numbers = find_horizontal_lines(executive_field, telephone_numbers);

						boost::remove_erase_if(telephone_numbers,
							[&executive_telephone_numbers](const std::pair<cv::Rect, std::wstring>& telephone_number) {
							for (const auto& executive_telephone_number : executive_telephone_numbers) {
								if (to_rect(executive_telephone_number) == to_rect(telephone_number))
									return true;
							}

							return false;
						});
					}
				}

				if (fields.find(L"담당자") != fields.end()) {
					if (telephone_numbers.size() > 1) {
						for (const auto& manager_field : fields.at(L"담당자")) {
							const auto cleaned_telephone_numbers = find_horizontal_lines(manager_field, telephone_numbers);

							if (cleaned_telephone_numbers.size() == 1)
								return std::get<1>(cleaned_telephone_numbers[0]);
						}
					}
				}

				return telephone_numbers.empty() ? L"" : std::get<1>(telephone_numbers[0]);
			}

			static std::wstring
				extract_fax_number(const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
				const std::vector<block>& blocks, const std::wstring& telephone_number, const cv::Size& image_size)
			{
				if (fields.find(L"팩스번호") == fields.end())
					return L"";

				std::vector<std::pair<cv::Rect, std::wstring>> fax_numbers;

				if (category == L"Financial Transaction Information Request"
					|| category == L"Debt Repayment Inquiry"
					|| category == L"Bottom Contact Info Page"
					|| category == L"FTIR Next Page With Contact Info"
					|| category == L"IDCard Of A PO With Contact Info") {
					std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>> correct_fields;
					for (const auto& number_field : fields.at(L"팩스번호")) {
						const auto& number = std::get<1>(number_field);
						if (number.find(L"수신") != std::wstring::npos) {
							continue;
						}

						bool has_incorrect_word = false;
						auto left_lines = find_left_lines(number_field, blocks, 0.5, 0.0, 100);
						for (const auto& line : left_lines) {
							const auto str = to_wstring(line);
							if (str.find(L"수신") != std::wstring::npos) {
								has_incorrect_word = true;
							}
						}

						if (!has_incorrect_word)
							correct_fields.push_back(number_field);
					}

					std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>> number_fields = { std::make_pair(L"팩스번호", correct_fields) };

					fax_numbers = extract_field_values(number_fields.at(L"팩스번호"), blocks,
						std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						preprocess_fax_number,
						create_extract_function(L"(((?:0(?:2|3[1-3]|4[1-4]|5[0-5]\\d?|6[1-4]))[)-]{0,2})?([0-9-]{7,11}))"),
						postprocess_fax_number);

					if (fax_numbers.empty()) {
						fax_numbers = extract_field_values(number_fields.at(L"팩스번호"), blocks,
							search_self,
							preprocess_fax_number,
							create_extract_function(L"(((?:0(?:2|3[1-3]|4[1-4]|5[0-5]\\d?|6[1-4]))[)-]{0,2})?([0-9-]{7,11}))"),
							postprocess_fax_number);
					}
				}
				else if (category == L"Cover Page Top Down Info") {
					std::vector<line> bottom_lines;
					std::copy_if(std::begin(blocks[0]), std::end(blocks[0]), std::back_inserter(bottom_lines), [&](const line& line) {
						const auto line_rect = to_rect(line);
						return line_rect.y > image_size.height * 0.77;
					});

					fax_numbers = extract_field_values(fields.at(L"팩스번호"), std::vector<block> {bottom_lines},
						std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						preprocess_fax_number,
						create_extract_function(L"(((?:0(?:2|3[1-3]|4[1-4]|5[0-5]\\d?|6[1-4]))[)-]{0,2})?([0-9-]{7,11}))"),
						postprocess_fax_number);

					if (fax_numbers.empty()) {
						fax_numbers = extract_field_values(fields.at(L"팩스번호"), std::vector<block> {bottom_lines},
							search_self,
							preprocess_fax_number,
							create_extract_function(L"(((?:0(?:2|3[1-3]|4[1-4]|5[0-5]\\d?|6[1-4]))[)-]{0,2})?([0-9-]{7,11}))"),
							postprocess_fax_number);
					}
				}

				if (fax_numbers.empty())
					return L"";

				boost::remove_erase_if(fax_numbers, [&telephone_number](const std::pair<cv::Rect, std::wstring>& fax_number) {
					return std::get<0>(fax_number).y < 120 || std::get<1>(fax_number) == telephone_number;
				});

				if (fields.find(L"책임자") != fields.end()) {
					for (const auto& executive_field : fields.at(L"책임자")) {
						const auto executive_fax_numbers = find_horizontal_lines(executive_field, fax_numbers);

						boost::remove_erase_if(fax_numbers, [&executive_fax_numbers](const std::pair<cv::Rect, std::wstring>& fax_number) {
							for (const auto& executive_fax_number : executive_fax_numbers) {
								if (to_rect(executive_fax_number) == to_rect(fax_number))
									return true;
							}

							return false;
						});
					}
				}


				if (fax_numbers.size() > 1) {
					for (const auto& fax_number : fax_numbers) {
						if (std::get<1>(fax_number).find(L"050") == 0)
							return std::get<1>(fax_number);
					}

					if (fields.find(L"담당자") != fields.end()) {
						for (const auto& manager_field : fields.at(L"담당자")) {
							const auto cleaned_fax_numbers = find_horizontal_lines(manager_field, fax_numbers);

							if (cleaned_fax_numbers.size() == 1)
								return std::get<1>(cleaned_fax_numbers[0]);
						}
					}
				}

				return fax_numbers.empty() ? L"" : std::get<1>(fax_numbers[0]);
			}

			static std::wstring
				extract_email_address(const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>&
				fields,
				const std::vector<block>& blocks, const cv::Size& image_size)
			{
				if (category != L"FTIR Next Page With Email" && fields.find(L"이메일주소") == fields.end())
					return L"";

				std::vector<std::pair<cv::Rect, std::wstring>> email_addresses;

				if (category == L"Financial Transaction Information Request"
					|| category == L"Bottom Contact Info Page"
					|| category == L"FTIR Next Page With Contact Info") {
					email_addresses = extract_field_values(fields.at(L"이메일주소"), blocks,
						search_self,
						std::bind(&fax_document_recognizer::preprocess_email_address, std::placeholders::_1),
						create_extract_function(L"([ A-Z0-9._%+-]+@[A-Z0-9.-]+\\.(?:KR|COM|ORG))"),
						postprocess_email_address);
				}
				else if (category == L"Debt Repayment Inquuiry"
					|| category == L"IDCard Of A PO With Contact Info") {
					email_addresses = extract_field_values(fields.at(L"이메일주소"), blocks,
						search_self,
						std::bind(&fax_document_recognizer::preprocess_email_address, std::placeholders::_1),
						create_extract_function(L"([ A-Z0-9._%+-]+@[A-Z0-9.-]+\\.(?:KR|COM|ORG))"),
						postprocess_email_address);

					if (email_addresses.empty()) {
						email_addresses = extract_field_values(fields.at(L"이메일주소"), blocks,
							std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
							std::bind(&fax_document_recognizer::preprocess_email_address, std::placeholders::_1),
							create_extract_function(L"([ A-Z0-9._%+-]+@[A-Z0-9.-]+\\.(?:KR|COM|ORG))"),
							postprocess_email_address);
					}
				}
				else if (category == L"Cover Page Top Down Info") {
					std::vector<line> bottom_lines;
					std::copy_if(std::begin(blocks[0]), std::end(blocks[0]), std::back_inserter(bottom_lines), [&](const line& line) {
						const auto line_rect = to_rect(line);
						return line_rect.y > image_size.height * 0.77;
					});

					email_addresses = extract_field_values(std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>{
						std::make_tuple(cv::Rect(), L"", cv::Range())
					}, std::vector<block> {bottom_lines},
					default_search,
					std::bind(&fax_document_recognizer::preprocess_email_address, std::placeholders::_1),
					create_extract_function(L"([ A-Z0-9._%+-]+@[A-Z0-9.-]+\\.(?:KR|COM|ORG))"),
					postprocess_email_address);

					email_addresses = extract_field_values(fields.at(L"이메일주소"), std::vector<block> {bottom_lines},
						std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						std::bind(&fax_document_recognizer::preprocess_email_address, std::placeholders::_1),
						create_extract_function(L"([ A-Z0-9._%+-]+@[A-Z0-9.-]+\\.(?:KR|COM|ORG))"),
						postprocess_email_address);

					if (email_addresses.empty()) {
						email_addresses = extract_field_values(fields.at(L"이메일주소"), std::vector<block> {bottom_lines},
							search_self,
							std::bind(&fax_document_recognizer::preprocess_email_address, std::placeholders::_1),
							create_extract_function(L"([ A-Z0-9._%+-]+@[A-Z0-9.-]+\\.(?:KR|COM|ORG))"),
							postprocess_email_address);
					}
				}

				if (email_addresses.empty()) {
					email_addresses = extract_field_values(std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>{
						std::make_tuple(cv::Rect(), L"", cv::Range())
					}, blocks,
					default_search,
					std::bind(&fax_document_recognizer::preprocess_email_address, std::placeholders::_1),
					create_extract_function(L"([ A-Z0-9._%+-]+@[A-Z0-9.-]+\\.(?:KR|COM|ORG))"),
					postprocess_email_address);
				}

				if (email_addresses.empty())
					return L"";

				if (fields.find(L"책임자") != fields.end()) {
					for (const auto& executive_field : fields.at(L"책임자")) {
						const auto executive_email_addresses = find_horizontal_lines(executive_field, email_addresses);

						boost::remove_erase_if(email_addresses,
							[&executive_email_addresses](const std::pair<cv::Rect, std::wstring>& email_address) {
							for (const auto& executive_email_address : executive_email_addresses) {
								if (to_rect(executive_email_address) == to_rect(email_address))
									return true;
							}

							return false;
						});
					}
				}

				if (email_addresses.size() > 1 && fields.find(L"담당자") != fields.end()) {
					for (const auto& manager_field : fields.at(L"담당자")) {
						const auto cleaned_email_addresses = find_horizontal_lines(manager_field, email_addresses);

						if (cleaned_email_addresses.size() == 1)
							return std::get<1>(cleaned_email_addresses[0]);
					}
				}

				return email_addresses.empty() ? L"" : std::get<1>(email_addresses[0]);
			}

			static std::wstring
				extract_grace_period(const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
				const std::vector<block>& blocks, const cv::Size& image_size)
			{
				if (fields.find(L"유예기간") == fields.end())
					return DEFAULT_FAX_COLUMN_VALUE_FOR_PRD;

				std::wstring grace_period = DEFAULT_FAX_COLUMN_VALUE_FOR_PRD;

				if (category == L"Financial Transaction Information Request") {
					auto grace_periods = extract_field_values(fields.at(L"유예기간"), blocks,
						std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						default_preprocess,
						create_extract_function(L"([0-9]+(?:일|개?[월원워]{1}|년|개))"),
						default_postprocess);

					if (grace_periods.empty()) {
						grace_periods = extract_field_values(fields.at(L"유예기간"), blocks,
							search_self,
							default_preprocess,
							create_extract_function(L"([0-9]+(?:일|개[월원워]{1}|년))"),
							default_postprocess);
					}

					if (grace_periods.empty()) {
						grace_periods = extract_field_values(fields.at(L"유예기간"), blocks,
							std::bind(find_right_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
							default_preprocess,
							create_extract_function(L"([0-9]+(?:일|개[월원워]{1}|년))"),
							default_postprocess);
					}

					if (grace_periods.empty()) {
						auto grace_periods_zero = extract_field_values(fields.at(L"유예기간"), blocks,
							search_self,
							default_preprocess,
							create_extract_function(L"([없업엇엄][옴은운]|[생셍][략락낙]|[즉슥]시)"),
							default_postprocess);

						if (grace_periods_zero.empty()) {
							grace_periods_zero = extract_field_values(fields.at(L"유예기간"), blocks,
								std::bind(find_right_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
								default_preprocess,
								create_extract_function(L"([없업엇엄][옴은운]|[생셍][략락낙]|[즉슥]시)"),
								default_postprocess);
						}

						if (!grace_periods_zero.empty()) {
							grace_periods.emplace_back(std::make_pair(cv::Rect(), L"0"));
						}
					}

					if (grace_periods.empty()) {
						if (!fields.at(L"유예기간").empty()) {
							grace_period = DEFAULT_FAX_COLUMN_VALUE_FOR_PRD;
						}
					}
					else {
						grace_period = boost::regex_replace(std::get<1>(grace_periods[0]), boost::wregex(L"[가-힝]"), L"");
						if (grace_period.size() == 0) {
							grace_period = DEFAULT_FAX_COLUMN_VALUE_FOR_PRD;
						}
					}
				}
				else if (category == L"Debt Repayment Inquiry") {
					grace_period = L"0";
				}

				return grace_period;
			}

			static std::wstring
				extract_document_number(const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>&
				fields,
				const std::vector<block>& blocks, const cv::Size& image_size)
			{
				if (fields.find(L"문서번호") == fields.end())
					return L"";

				std::vector<std::pair<cv::Rect, std::wstring>> document_numbers;

				if (category == L"Financial Transaction Information Request"
					|| category == L"Debt Repayment Inquiry") {
					document_numbers = extract_field_values(fields.at(L"문서번호"), blocks,
						std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						default_preprocess,
						default_extract,
						std::bind(&fax_document_recognizer::postprocess_document_number, std::placeholders::_1));

					if (document_numbers.empty()) {
						document_numbers = extract_field_values(fields.at(L"문서번호"), blocks,
							search_self,
							default_preprocess,
							default_extract,
							std::bind(&fax_document_recognizer::postprocess_document_number, std::placeholders::_1));
					}
				}

				return document_numbers.empty() ? L"" : std::get<1>(document_numbers[0]);
			}

			static std::pair<cv::Rect, std::wstring>
				extract_warrant_number(const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>&
				fields,
				const std::vector<block>& blocks, const cv::Size& image_size)
			{
				std::pair<cv::Rect, std::wstring> warrant_number;

				if (fields.find(L"영장번호") == fields.end())
					return warrant_number;

				std::vector<std::pair<cv::Rect, std::wstring>> warrant_numbers;

				if (category == L"Financial Transaction Information Request"
					|| category == L"Confiscation Search Verification Warrent") {
					warrant_numbers = extract_field_values(fields.at(L"영장번호"), blocks,
						std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						default_preprocess,
						create_extract_function(L"(([0-9]{4}[- ]{1}[0-9]{3,5}(?:-[0-9]{1}){0,2})|(20[0-9]{2}년[ ]{0,2}제[0-9]{4}호(:?[(]?[0-9-]{4,6}[)]?)?))"),
						std::bind(&fax_document_recognizer::postprocess_warrant_number, std::placeholders::_1));

					if (warrant_numbers.empty()) {
						warrant_numbers = extract_field_values(fields.at(L"영장번호"), blocks,
							search_self,
							default_preprocess,
							create_extract_function(L"(([0-9]{4}[- ]{1}[0-9]{3,5}(?:-[0-9]{1}){0,2})|(20[0-9]{2}년[ ]{0,2}제[0-9]{4}호(:?[(]?[0-9-]{4,6}[)]?)?))"),
							std::bind(&fax_document_recognizer::postprocess_warrant_number, std::placeholders::_1));
					}

					if (category == L"Confiscation Search Verification Warrent" && warrant_numbers.empty() && fields.find(L"죄명") != fields.end()) {
						warrant_numbers = extract_field_values(fields.at(L"죄명"), blocks,
							std::bind(find_nearest_left_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
							default_preprocess,
							create_extract_function(L"(([0-9]{4}[- ]{1}[0-9]{3,5}(?:-[0-9]{1}){0,2})|(20[0-9]{2}년[ ]{0,2}제[0-9]{4}호(:?[(]?[0-9-]{4,6}[)]?)?))"),
							std::bind(&fax_document_recognizer::postprocess_warrant_number, std::placeholders::_1));
					}
				}

				return warrant_numbers.empty() ? warrant_number : warrant_numbers[0];
			}

			static std::wstring
				extract_crime_name(const configuration& configuration, const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>&
				fields,
				const std::vector<block>& blocks, std::pair<cv::Rect, std::wstring> warrant_number_value, const cv::Size& image_size)
			{
				if (fields.find(L"죄명") == fields.end())
					return L"";

				std::vector<std::pair<cv::Rect, std::wstring>> crime_names;

				if (category == L"Confiscation Search Verification Warrent") {
					// 죄명 필드는 키워드 "죄명" 보정 시, 너무 많은 오류가 있어서 먼저 영장번호 키워드와 우측 라인 중에 검출된 죄명 키워드를 사용
					for (const auto& warrant_number_field : fields.at(L"영장번호")) {
						const auto right_lines_of_warrant_number = find_right_lines(warrant_number_field, blocks);

						if (right_lines_of_warrant_number.empty())
							continue;

						for (const auto& crime_name_field : fields.at(L"죄명")) {
							for (auto i = 0; i < right_lines_of_warrant_number.size(); i++) {
								if (std::get<0>(right_lines_of_warrant_number[i]) == std::get<0>(crime_name_field)) {
									const std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>> suspected_crime_name_field = { crime_name_field };

									// "죄명" 바로 우측 1줄 짜리 라인에서 죄명 찾기
									auto suspected_crime_names = extract_field_values(suspected_crime_name_field, blocks,
										std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
										preprocess_crime_name,
										default_extract,
										default_postprocess);

									boost::remove_erase_if(suspected_crime_names, [](const std::pair<cv::Rect, std::wstring>& field) {
										auto text = std::wstring(std::get<1>(field));
										text = boost::regex_replace(text, boost::wregex(L"[짐집]{1}[행앵]{1}지[휘위의]{1}"), L"");
										return text.size() < 2;
									});

									// "죄명" 라인에서 죄명 찾기
									if (suspected_crime_names.empty()) {
										suspected_crime_names = extract_field_values(suspected_crime_name_field, blocks,
											search_self,
											preprocess_crime_name,
											default_extract,
											default_postprocess);
									}

									if (!suspected_crime_names.empty() && !std::get<1>(suspected_crime_names[0]).empty())
										return std::get<1>(suspected_crime_names[0]);

									// "죄명" 바로 우측 2줄 짜리 라인에서 죄명 찾기
									std::wstring suspected_crime_name;
									auto right_lines_of_crime_name = find_right_lines(crime_name_field, blocks, 0.1, 0.9, 100, true);
									for (auto& crime_name : right_lines_of_crime_name) {
										suspected_crime_name += boost::regex_replace(to_wstring(crime_name), boost::wregex(L"[^가-힣]"), L"");
										suspected_crime_name = boost::regex_replace(suspected_crime_name, boost::wregex(L"[짐집]{1}[행앵]{1}지[휘위의]{1}"), L"");
									}

									if (suspected_crime_name.size() > 1)
										return suspected_crime_name;
								}
							}
						}
					}

					// 이미 인식된 영장번호가 있으면 그 우측에서 죄명 키워드 찾기
					if (!std::get<1>(warrant_number_value).empty()) {
						const auto warrant_number_field = std::make_tuple(std::get<0>(warrant_number_value), std::get<1>(warrant_number_value), cv::Range());
						const auto right_lines_of_warrant_number = find_right_lines(warrant_number_field, blocks);

						for (const auto& crime_name_field : fields.at(L"죄명")) {
							for (auto i = 0; i < right_lines_of_warrant_number.size(); i++) {
								if (std::get<0>(right_lines_of_warrant_number[i]) == std::get<0>(crime_name_field)) {
									// "죄명" 바로 우측 1줄 짜리 라인에서 죄명 찾기
									const std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>> suspected_crime_name_field = { crime_name_field };
									auto suspected_crime_names = extract_field_values(suspected_crime_name_field, blocks,
										std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
										preprocess_crime_name,
										default_extract,
										default_postprocess);

									// "죄명" 라인에서 죄명 찾기
									if (suspected_crime_names.empty()) {
										suspected_crime_names = extract_field_values(suspected_crime_name_field, blocks,
											search_self,
											preprocess_crime_name,
											default_extract,
											default_postprocess);
									}

									if (!suspected_crime_names.empty() && !std::get<1>(suspected_crime_names[0]).empty())
										return std::get<1>(suspected_crime_names[0]);

									// "죄명" 바로 우측 2줄 짜리 라인에서 죄명 찾기
									std::wstring suspected_crime_name;
									auto right_lines_of_crime_name = find_right_lines(crime_name_field, blocks, 0.1, 0.9, 100, true);
									for (auto& crime_name : right_lines_of_crime_name) {
										suspected_crime_name += boost::regex_replace(to_wstring(crime_name), boost::wregex(L"[^가-힣]"), L"");
										suspected_crime_name = boost::regex_replace(suspected_crime_name, boost::wregex(L"[짐집]{1}[행앵]{1}지[휘위의]{1}"), L"");
									}

									if (suspected_crime_name.size() > 1)
										return suspected_crime_name;
								}
							}
						}
					}
				}

				return L"";
			}

			static std::wstring
				convert_crime_name_to_code(std::wstring& crime_name_str)
			{

				const std::unordered_map<std::wstring, std::vector<std::wstring>> crime_name_codes = {
					{ L"01", { L"뇌물", L"알선수재", L"수뢰" } },
					{ L"02", { L"횡령" } },
					{ L"03", { L"사기", L"사기자금" } },
					{ L"04", { L"정치자금법위반" } },
					{ L"05", { L"상습도박", L"복표" } },
					{ L"06", { L"통화", L"유가증권위조" } },
					{ L"07", { L"범죄수익" } },
					{ L"08", { L"자금세탁" } },
					{ L"09", { L"절도", L"장물", L"도난" } },
					{ L"10", { L"조직범죄" } },
					{ L"11", { L"증권거래법위반" } },
					{ L"12", { L"밀수", L"마약범죄" } },
					{ L"13", { L"테러자금조달" } },
					{ L"14", { L"기타" } },
				};

				for (const auto& crime_name_code : crime_name_codes) {
					for (const auto& crime_name : std::get<1>(crime_name_code)) {
						if (crime_name_str.find(crime_name) != std::wstring::npos)
							return std::get<0>(crime_name_code);
					}
				}

				return L"14";
			}

			static std::wstring
				extract_use_purpose(const configuration& configuration, const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>&
				fields,
				const std::vector<block>& blocks, const cv::Size& image_size)
			{
				if (fields.find(L"사용목적") == fields.end())
					return L"";

				std::vector<std::pair<cv::Rect, std::wstring>> use_purposes;

				use_purposes = extract_field_values(fields.at(L"사용목적"), blocks,
				std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
					default_preprocess,
					create_extract_function(configuration, L"purpose", 2),
					default_postprocess);

				if (use_purposes.empty()) {
					use_purposes = extract_field_values(fields.at(L"사용목적"), blocks,
						search_self,
						default_preprocess,
						create_extract_function(configuration, L"purpose", 2),
						default_postprocess);
				}

				return use_purposes.empty() ? L"" : std::get<1>(use_purposes[0]);
			}

			static std::pair<cv::Rect, std::wstring>
				extract_manager_position(const configuration& configuration, const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
				const std::vector<block>& blocks, const cv::Size& image_size, const std::wstring request_organ)
			{
				std::pair<cv::Rect, std::wstring> manager_position;

				if (fields.find(L"직책") == fields.end())
					return manager_position;

				if (category == L"Financial Transaction Information Request"
					|| category == L"Debt Repayment Inquiry") {
					std::vector<std::pair<cv::Rect, std::wstring>> suspected_horizontal_manager_position;
					if (request_organ == L"감사원") {
						suspected_horizontal_manager_position = extract_field_values(fields.at(L"담당자"), blocks,
							std::bind(find_right_lines, std::placeholders::_1, std::placeholders::_2, 0.1, 0.9, 100, true),
							default_preprocess,
							default_extract,
							std::bind(&fax_document_recognizer::postprocess_position, std::placeholders::_1));
					}
					else {
						suspected_horizontal_manager_position = extract_field_values(fields.at(L"담당자"), blocks,
							std::bind(find_right_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
							default_preprocess,
							default_extract,
							std::bind(&fax_document_recognizer::postprocess_position, std::placeholders::_1));
						if (suspected_horizontal_manager_position.empty()) {
							suspected_horizontal_manager_position = extract_field_values(fields.at(L"담당자"), blocks,
								std::bind(find_right_lines, std::placeholders::_1, std::placeholders::_2, 0.1, 0.9, 100, true),
								default_preprocess,
								default_extract,
								std::bind(&fax_document_recognizer::postprocess_position, std::placeholders::_1));
						}
					}

					auto suspected_vertical_manager_position = extract_field_values(fields.at(L"직책"), blocks,
						std::bind(find_down_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						default_preprocess,
						default_extract,
						std::bind(&fax_document_recognizer::postprocess_position, std::placeholders::_1));

					for (auto i = 0; i < suspected_horizontal_manager_position.size(); i++) {
						for (auto j = 0; j < suspected_vertical_manager_position.size(); j++) {
							if (std::get<0>(suspected_horizontal_manager_position[i]) == std::get<0>(suspected_vertical_manager_position[j])
								&& std::get<1>(suspected_vertical_manager_position[j]).size() > std::get<1>(manager_position).size()) {
								manager_position = suspected_vertical_manager_position[j];
							}
						}
					}

					if (std::get<1>(manager_position).empty()) {
						std::vector<std::pair<cv::Rect, std::wstring>> manager_positions;
						if (category == L"Financial Transaction Information Request") {
							manager_positions = extract_field_values(fields.at(L"직책"), blocks,
								std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
								default_preprocess,
								default_extract,
								std::bind(&fax_document_recognizer::postprocess_position, std::placeholders::_1));

							if (manager_positions.empty()) {
								manager_positions = extract_field_values(fields.at(L"직책"), blocks,
									search_self,
									default_preprocess,
									default_extract,
									std::bind(&fax_document_recognizer::postprocess_position, std::placeholders::_1));
							}

							if (manager_positions.empty() && fields.at(L"직책").empty()) {
								auto suspected_horizontal_requester = extract_field_values(fields.at(L"요구자"), blocks,
									std::bind(find_right_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
									default_preprocess,
									default_extract,
									default_postprocess);
								boost::remove_erase_if(suspected_horizontal_requester, [&fields](const std::pair<cv::Rect, std::wstring>& suspected_requester) {
									for (const auto& field : fields.at(L"성명")) {
										if (std::get<0>(field) == std::get<0>(suspected_requester))
											return true;
									}

									return false;
								});

								auto suspected_vertical_request_date = extract_field_values(fields.at(L"요구일자"), blocks,
									std::bind(find_down_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
									default_preprocess,
									default_extract,
									default_postprocess);
								boost::remove_erase_if(suspected_vertical_request_date, [&fields](const std::pair<cv::Rect, std::wstring>& suspected_date) {
									for (const auto& field : fields.at(L"성명")) {
										if (std::get<0>(field) == std::get<0>(suspected_date))
											return true;
									}

									return false;
								});

								const auto paper_size = image_size;
								boost::remove_erase_if(suspected_vertical_request_date, [&paper_size](const std::pair<cv::Rect, std::wstring>& field) {
									return std::get<0>(field).width > paper_size.width * 0.3;
								});

								for (auto i = 0; i < suspected_horizontal_requester.size(); i++) {
									for (auto j = 0; j < suspected_vertical_request_date.size(); j++) {
										if (std::get<0>(suspected_horizontal_requester[i]) == std::get<0>(suspected_vertical_request_date[j])) {
											const auto suspected_manager_position_field = {
												std::make_tuple(std::get<0>(suspected_vertical_request_date[j]), std::get<1>(suspected_vertical_request_date[j]), cv::Range()) };
											suspected_vertical_manager_position = extract_field_values(suspected_manager_position_field, blocks,
												std::bind(find_down_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
												default_preprocess,
												default_extract,
												std::bind(&fax_document_recognizer::postprocess_position, std::placeholders::_1));

											for (auto m = 0; m < suspected_horizontal_manager_position.size(); m++) {
												for (auto n = 0; n < suspected_vertical_manager_position.size(); n++) {
													if (std::get<0>(suspected_horizontal_manager_position[m]) == std::get<0>(suspected_vertical_manager_position[n])) {
														manager_position = suspected_vertical_manager_position[n];
													}
												}
											}
										}
									}
								}
							}
						}
						else if (category == L"Debt Repayment Inquiry") {
							manager_positions = extract_field_values(fields.at(L"담당자"), blocks,
								search_self,
								default_preprocess,
								create_extract_function(configuration, L"position", 0),
								default_postprocess);
						}

						if (!manager_positions.empty())
							manager_position = manager_positions[0];
					}
				}
				else if (category == L"IDCard Of A PO With Contact Info") {
					auto manager_positions = extract_field_values(fields.at(L"직책"), blocks,
						std::bind(find_right_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						default_preprocess,
						create_extract_function(configuration, L"position", 0),
						std::bind(&fax_document_recognizer::postprocess_position, std::placeholders::_1));

					if (manager_positions.empty()) {
						manager_positions = extract_field_values(fields.at(L"직책"), blocks,
							search_self,
							default_preprocess,
							create_extract_function(configuration, L"position", 0),
							std::bind(&fax_document_recognizer::postprocess_position, std::placeholders::_1));
					}

					if (manager_positions.empty()) {
						manager_positions = extract_field_values(
							std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>{std::make_tuple(cv::Rect(), L"", cv::Range())},
							blocks,
							default_search,
							default_preprocess,
							create_extract_function(configuration, L"position", 0),
							std::bind(&fax_document_recognizer::postprocess_position, std::placeholders::_1));

					}

					if (!manager_positions.empty())
						manager_position = manager_positions[0];
				}

				if (!std::get<1>(manager_position).empty()) {
					std::get<1>(manager_position) = extract_manager_position_using_dictionary(configuration, std::get<1>(manager_position), image_size);
				}

				return manager_position;
			}

			static std::wstring
				extract_manager_name(const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
				const std::vector<block>& blocks,
				std::pair<cv::Rect, std::wstring> manager_position_value, std::pair<cv::Rect, std::wstring> executive_name_value, const cv::Size& image_size, const std::wstring request_organ)
			{
				if (fields.find(L"담당자") == fields.end())
					return L"";

				if (category == L"Financial Transaction Information Request"
					|| category == L"Debt Repayment Inquiry") {
					auto suspected_horizontal_manager_name = extract_field_values(fields.at(L"담당자"), blocks,
						std::bind(find_right_lines, std::placeholders::_1, std::placeholders::_2, 0.1, 0.9, 100, true),
						preprocess_name,
						default_extract,
						std::bind(&fax_document_recognizer::postprocess_name, std::placeholders::_1));

					boost::remove_erase_if(suspected_horizontal_manager_name, [](const std::pair<cv::Rect, std::wstring>& field) {
						auto text = std::wstring(std::get<1>(field));
						boost::regex_replace(text, boost::wregex(L"[가-힝]"), L"");
						return text.size() < 2 || text.find(L"전화") == 0 || text.find(L"메일") != std::wstring::npos;
					});

					auto suspected_vertical_manager_name = extract_field_values(fields.at(L"성명"), blocks,
						std::bind(find_down_lines, std::placeholders::_1, std::placeholders::_2, 0.2, 0.8, 100, true),
						preprocess_name,
						default_extract,
						std::bind(&fax_document_recognizer::postprocess_name, std::placeholders::_1));

					auto suspected_vertical_manager_position = extract_field_values(fields.at(L"직책"), blocks,
						std::bind(find_down_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						default_preprocess,
						default_extract,
						std::bind(&fax_document_recognizer::postprocess_position, std::placeholders::_1));
					boost::remove_erase_if(suspected_vertical_manager_name, [&suspected_vertical_manager_position](const std::pair<cv::Rect, std::wstring>& field) {
						auto manager_name_rect = std::get<0>(field);
						for (auto vertical_manager_position : suspected_vertical_manager_position) {
							auto manager_position_rect = std::get<0>(vertical_manager_position);

							const auto collision = manager_name_rect & manager_position_rect;
							if (!collision.empty())
								return true;
						}

						return false;
					});

					std::wstring manager_name;
					for (auto i = 0; i < suspected_horizontal_manager_name.size(); i++) {
						for (auto j = 0; j < suspected_vertical_manager_name.size(); j++) {
							if (std::get<0>(suspected_horizontal_manager_name[i]) == std::get<0>(suspected_vertical_manager_name[j])) {
								auto manager_postition_rect = std::get<0>(manager_position_value);
								auto executive_name_rect = std::get<0>(executive_name_value);
								if (manager_postition_rect == std::get<0>(suspected_horizontal_manager_name[i])
									|| manager_postition_rect == std::get<0>(suspected_vertical_manager_name[j])
									|| executive_name_rect == std::get<0>(suspected_horizontal_manager_name[i])
									|| executive_name_rect == std::get<0>(suspected_vertical_manager_name[j]))
									continue;

								if (executive_name_rect.empty())
									return std::get<1>(suspected_vertical_manager_name[j]);

								if (executive_name_rect.y > 0 && std::get<0>(suspected_vertical_manager_name[j]).y < executive_name_rect.y) {
									if (manager_name.find(std::get<1>(suspected_vertical_manager_name[j])) == std::wstring::npos)
										manager_name += std::get<1>(suspected_vertical_manager_name[j]);
								}
							}
						}
					}
					if (!manager_name.empty())
						return manager_name;

					// 1줄 짜리 성명
					std::vector<std::pair<cv::Rect, std::wstring>> manager_names;
					if (category == L"Financial Transaction Information Request") {
						manager_names = extract_field_values(fields.at(L"성명"), blocks,
							std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
							default_preprocess,
							default_extract,
							std::bind(&fax_document_recognizer::postprocess_name, std::placeholders::_1));

						if (manager_names.empty()) {
							manager_names = extract_field_values(fields.at(L"성명"), blocks,
								search_self,
								default_preprocess,
								default_extract,
								std::bind(&fax_document_recognizer::postprocess_name, std::placeholders::_1));
						}

						if (manager_names.empty()) {
							if (!fields.at(L"담당자").empty() && !fields.at(L"성명").empty()) {
								if (request_organ == L"감사원" && !suspected_vertical_manager_name.empty()) {
									return std::get<1>(suspected_vertical_manager_name.front());
								}
								else if (!suspected_horizontal_manager_name.empty()) {
									return std::get<1>(suspected_horizontal_manager_name.back());
								}
							}
						}
					}

					if (category == L"Debt Repayment Inquiry") {
						manager_names = extract_field_values(fields.at(L"담당자"), blocks,
							search_self,
							default_preprocess,
							default_extract,
							std::bind(&fax_document_recognizer::postprocess_name, std::placeholders::_1));

						if (manager_names.empty()) {
							manager_names = extract_field_values(fields.at(L"담당자"), blocks,
								std::bind(find_right_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
								preprocess_name,
								default_extract,
								std::bind(&fax_document_recognizer::postprocess_name, std::placeholders::_1));
						}
					}

					if (!manager_names.empty())
						return std::get<1>(manager_names[0]);
				}
				else if (category == L"FTIR Next Page With Contact Info") {
					auto manager_names = extract_field_values(fields.at(L"담당자"), blocks,
						search_self,
						default_preprocess,
						default_extract,
						std::bind(&fax_document_recognizer::postprocess_name, std::placeholders::_1));

					if (manager_names.empty()) {
						manager_names = extract_field_values(fields.at(L"담당자"), blocks,
							std::bind(find_right_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
							preprocess_name,
							default_extract,
							std::bind(&fax_document_recognizer::postprocess_name, std::placeholders::_1));
					}

					if (!manager_names.empty())
						return std::get<1>(manager_names[0]);
				}
				else if (category == L"IDCard Of A PO With Contact Info") {
					auto manager_names = extract_field_values(fields.at(L"담당자"), blocks,
						search_self,
						preprocess_name,
						default_extract,
						std::bind(&fax_document_recognizer::postprocess_name, std::placeholders::_1));

					if (manager_names.empty()) {
						manager_names = extract_field_values(fields.at(L"담당자"), blocks,
							std::bind(find_right_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
							default_preprocess,
							default_extract,
							std::bind(fax_document_recognizer::postprocess_name, std::placeholders::_1));
					}

					if (manager_names.empty()) {
						manager_names = extract_field_values(fields.at(L"직책"), blocks,
							std::bind(find_right_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
							default_preprocess,
							default_extract,
							std::bind(&fax_document_recognizer::postprocess_name, std::placeholders::_1));
					}

					if (manager_names.empty()) {
						manager_names = extract_field_values(fields.at(L"직책"), blocks,
							search_self,
							preprocess_name,
							default_extract,
							std::bind(&fax_document_recognizer::postprocess_name, std::placeholders::_1));
					}

					if (manager_names.empty())
						return L"";

					int max_height_idx = 0;
					for (int i = 0; i < manager_names.size(); i++) {
						auto line_rect = to_rect(manager_names[i]);
						max_height_idx = line_rect.height > max_height_idx ? i : max_height_idx;
					}

					return std::get<1>(manager_names[max_height_idx]);
				}

				return L"";
			}

			static std::pair<cv::Rect, std::wstring>
				extract_executive_name(const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
				const std::vector<block>& blocks, const cv::Size& image_size, const std::wstring request_organ)
			{
				std::pair<cv::Rect, std::wstring>executive_name;
				if (fields.find(L"책임자") == fields.end())
					return executive_name;

				if (category == L"Financial Transaction Information Request"
					|| category == L"Debt Repayment Inquiry") {
					const auto suspected_horizontal_executive_name = extract_field_values(fields.at(L"책임자"), blocks,
						std::bind(find_right_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						preprocess_name,
						default_extract,
						std::bind(&fax_document_recognizer::postprocess_name, std::placeholders::_1));

					const auto paper_size = image_size;
					std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>> correct_fields;
					for (const auto& name_field : fields.at(L"성명")) {
						if (to_rect(name_field).width < paper_size.width * 0.35)
							correct_fields.push_back(name_field);
					}

					std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>> name_fields = { std::make_pair(L"성명", correct_fields) };

					const auto suspected_vertical_executive_name = extract_field_values(name_fields.at(L"성명"), blocks,
						std::bind(find_down_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						preprocess_name,
						default_extract,
						std::bind(&fax_document_recognizer::postprocess_name, std::placeholders::_1));

					for (auto i = 0; i < suspected_horizontal_executive_name.size(); i++) {
						for (auto j = 0; j < suspected_vertical_executive_name.size(); j++) {
							if (std::get<0>(suspected_horizontal_executive_name[i]) == std::get<0>(suspected_vertical_executive_name[j])) {
								return suspected_vertical_executive_name[j];
							}
						}
					}

					if (category == L"Financial Transaction Information Request") {
						if (!fields.at(L"담당자").empty() && !fields.at(L"성명").empty()) {
							if (request_organ == L"감사원" && suspected_vertical_executive_name.size() > 2) {
								// return suspected_vertical_executive_name.front();[^가-힣]
								std::vector<std::pair<cv::Rect, std::wstring>> names;
								for (const auto& name : suspected_vertical_executive_name) {
									auto cleaned = boost::regex_replace(std::get<1>(name), boost::wregex(L"[^가-힣]"), L"");
									if (cleaned.size() >= 2)
										names.push_back(name);
								}

								if (names.size() >= 2)
									return names[1];
							}
							else if (!suspected_horizontal_executive_name.empty()) {
								return suspected_horizontal_executive_name.back();
							}
						}

						if (!fields.at(L"책임자").empty() && !fields.at(L"성명").empty() && !suspected_horizontal_executive_name.empty()) {
							return suspected_horizontal_executive_name.back();
						}
					}
					else if (category == L"Debt Repayment Inquiry") {
						for (const auto& field : fields.at(L"책임자")) {
							const auto field_value = std::get<1>(field);
							const auto field_range = std::get<2>(field);
							const auto name = postprocess_name(std::wstring(std::begin(field_value) + field_range.end, std::end(field_value)));

							if (!name.empty())
								return std::make_pair(cv::Rect(), name);
						}
					}
				}

				return executive_name;
			}

			static std::wstring
				extract_manager_position_using_dictionary(const configuration& configuration, const std::wstring suspected_manager_position, const cv::Size& image_size)
			{
				if (suspected_manager_position.empty())
					return L"";

				std::wstring manager_position = suspected_manager_position;
				int distance = manager_position.size() > 5 ? 2 : manager_position.size() > 3 ? 1 : 0;

				const auto keywords = get_dictionary_words(configuration, dictionaries_, L"position");
				const auto dictionary = build_spell_dictionary(configuration, L"position");
				aho_corasick::wtrie trie;
				build_trie(trie, keywords);

				const auto suggested = dictionary->Correct(manager_position);
				if (!suggested.empty() && suggested[0].distance <= distance)
					manager_position = suggested[0].term;

				auto matches = trie.parse_text(manager_position);
				if (matches.empty())
					return suspected_manager_position;

				return dictionary->Correct(matches.front().get_keyword())[0].term;
			}
		};


		class coa_document_recognizer : public recognizer
		{
		public:
			std::pair<std::wstring, int>
				recognize(const std::string& buffer, int languages, const std::string& secret) override
			{
				throw std::logic_error("not implmented");
			}
			std::pair<std::wstring, int>
				recognize(const std::string& buffer, const std::wstring& type, const std::string& secret) override
			{
				throw std::logic_error("not implmented");
			}
			std::unordered_map<std::wstring, std::vector<std::wstring>>
				recognize(const boost::filesystem::path& path, const std::string& secret) override
			{
				const auto configuration = load_configuration(L"coa");

				std::unordered_map<std::wstring, std::vector<std::unordered_map<std::wstring, std::vector<std::wstring>>>> fields;

				std::mutex locks;
				const auto fill_field = [&locks](std::unordered_map<std::wstring, std::vector<std::wstring>>& fields, const std::wstring& field,
					const std::vector<std::wstring>& value) {
					locks.lock();
					if (!value.empty())
						fields.emplace(std::make_pair(field, value));
					locks.unlock();
				};

				std::vector<std::wstring> files;
				if (boost::filesystem::is_directory(path)) {
					for (auto& entry : boost::filesystem::recursive_directory_iterator(path)) {
						const auto file = entry.path();
						const auto extension = boost::algorithm::to_lower_copy(file.extension().native());

						if (extension != L".png" && extension != L".jpg" && extension != L".tif" && extension != L".pdf")
							continue;

						files.emplace_back(boost::filesystem::absolute(file).native());
					}

					std::sort(std::begin(files), std::end(files), compareNat);
				}
				else {
					const auto extension = boost::algorithm::to_lower_copy(path.extension().native());

					if (extension != L".png" && extension != L".jpg" && extension != L".tif" && extension != L".pdf")
						return std::unordered_map<std::wstring, std::vector<std::wstring>>();

					files.emplace_back(boost::filesystem::absolute(path).native());
				}

				std::unordered_map<std::wstring, std::vector<std::wstring>> extracted_fields;

#ifdef LOG_USE_WOFSTREAM
				std::wofstream txt_file;
				txt_file.imbue(std::locale(std::locale::empty(), new std::codecvt_utf8<wchar_t, 0x10ffff, std::generate_header>));
				txt_file.open(__LOG_FILE_NAME__, std::wofstream::out | std::wofstream::app);
#endif

				try {
					// cv::parallel_for_(cv::Range(0, files.size()), [&](const cv::Range& range) {

					CComPtr<FREngine::IEngineLoader> loader;
					FREngine::IEnginePtr engine;
#if __USE_ENGINE__
					std::tie(loader, engine) = get_engine_object(configuration);
#endif
					FREngine::IClassificationEnginePtr classification_engine;
					FREngine::IModelPtr classification_model;
#if __USE_ENGINE__
					classification_engine = engine->CreateClassificationEngine();
					classification_model = classification_engine->
						CreateModelFromFile(get_classification_model(configuration).c_str());
#endif

					// for (auto i = range.start; i < range.end; i++) {
					for (auto i = 0; i < files.size(); i++) {
						memory_reader memory_reader(boost::filesystem::absolute(files[i]), secret);

						FREngine::IFRDocumentPtr document;
#if __USE_ENGINE__
						document = engine->CreateFRDocument();
						document->AddImageFileFromStream(&memory_reader, nullptr, nullptr, nullptr,
							boost::filesystem::path(files[i]).filename().native().c_str());

						auto page_preprocessing_params = engine->CreatePagePreprocessingParams();
						page_preprocessing_params->CorrectOrientation = VARIANT_TRUE;
						page_preprocessing_params->OrientationDetectionParams->put_OrientationDetectionMode(FREngine::OrientationDetectionModeEnum::ODM_Thorough);
						document->Preprocess(page_preprocessing_params, nullptr, nullptr, nullptr);

						if (document->Pages->Count < 1) {
							document->Close();
							continue;
						}
#endif

#ifdef LOG_USE_WOFSTREAM
						txt_file << L"-----------------------------------------------------" << std::endl;
						txt_file << L"File : " << files[i] << std::endl;
#endif

						auto class_name = classify_document(engine, configuration, classification_engine, classification_model, files[i], document);
						if (class_name.empty()) {
/*#ifdef __USE_ENGINE__
							document->Close();
#endif
							continue;*/
							class_name = L"OTHERS";
						}

						const std::set<std::wstring> processed_class_names{
							L"ALBEMARLE",
							L"BASF",
							L"BASF_B",
							L"CHIMEI",
							L"DONGSUNG",
							L"DUKSAN",
							L"LANXESS",
							L"LG HNH",
							L"NOF",
							L"POWERTECH",
							L"SKGC",
							L"SUNGU",
							L"TEASUNG CHEMICAL",
							L"UNID",
							L"WEIHAI",
							L"XIAMETER",
							L"OTHERS",
						};

						// 인식할 필요가 없는 문서는 pass
						if (processed_class_names.find(class_name) == processed_class_names.end()) {
							document->Close();
							continue;
						}

						std::vector<block> blocks;
						cv::Size image_size;
						std::tie(blocks, std::ignore, image_size) = recognize_document(engine, configuration, class_name, files[i], document);
						if (image_size.area() == 0)
							image_size = estimate_paper_size(blocks);
#if __USE_ENGINE__
						document->Close();
#endif

						const auto keywords = get_keywords(configuration, keywords_, class_name);
						const auto searched_fields = search_fields(class_name, keywords, blocks);

						if (class_name == L"LG HNH") {
							//auto company_name = extract_company_name(configuration, class_name, searched_fields, blocks, image_size);
							auto product_name = extract_product_name(configuration, class_name, searched_fields, blocks, image_size);
							auto lot_no_pair = extract_lot_no(configuration, class_name, searched_fields, blocks, image_size);
							auto date_pair = extract_date_of_production(configuration, class_name, searched_fields, blocks, image_size);
							auto lot_item_map = extract_items_unit_results(configuration, class_name, searched_fields, blocks, image_size, lot_no_pair, date_pair);

							std::vector<std::wstring> lot_no;
							std::transform(lot_no_pair.begin(), lot_no_pair.end(), std::back_inserter(lot_no), [](std::pair<cv::Rect, std::wstring>& pair) {
								return pair.second;
							});

							std::vector<std::wstring> date_of_production;
							std::transform(date_pair.begin(), date_pair.end(), std::back_inserter(date_of_production), [](std::pair<cv::Rect, std::wstring>& pair) {
								return pair.second;
							});

							std::vector<std::wstring> items;
							std::vector<std::wstring> units;
							std::vector<std::wstring> results;
							transform_list(lot_item_map, items, units, results);


							fill_field(extracted_fields, L"COMPANY NAME", std::vector<std::wstring>{L"LG Household & Health Care"});
							fill_field(extracted_fields, L"PRODUCT NAME", product_name);
							fill_field(extracted_fields, L"LOT NO", lot_no);
							fill_field(extracted_fields, L"DATE OF PRODUCTION", std::vector<std::wstring>{date_of_production});
							fill_field(extracted_fields, L"ITEMS", items);
							fill_field(extracted_fields, L"UNIT", units);
							fill_field(extracted_fields, L"RESULT", results);
						}
						else if (class_name == L"BASF") {
							//auto company_name = extract_company_name(configuration, class_name, searched_fields, blocks, image_size);
							auto product_name = extract_product_name(configuration, class_name, searched_fields, blocks, image_size);
							auto lot_no_pair = extract_lot_no(configuration, class_name, searched_fields, blocks, image_size);
							auto date_pair = extract_date_of_production(configuration, class_name, searched_fields, blocks, image_size);
							auto lot_item_map = extract_items_unit_results(configuration, class_name, searched_fields, blocks, image_size, lot_no_pair, date_pair);

							std::vector<std::wstring> lot_no;
							std::transform(lot_no_pair.begin(), lot_no_pair.end(), std::back_inserter(lot_no), [](std::pair<cv::Rect, std::wstring>& pair) {
								return pair.second;
							});

							std::vector<std::wstring> date_of_production;
							std::transform(date_pair.begin(), date_pair.end(), std::back_inserter(date_of_production), [](std::pair<cv::Rect, std::wstring>& pair) {
								return pair.second;
							});

							std::vector<std::wstring> items;
							std::vector<std::wstring> units;
							std::vector<std::wstring> results;
							transform_list(lot_item_map, items, units, results);

							fill_field(extracted_fields, L"COMPANY NAME", std::vector<std::wstring>{L"BASF Company Ltd."});
							fill_field(extracted_fields, L"PRODUCT NAME", product_name);
							fill_field(extracted_fields, L"LOT NO", lot_no);
							fill_field(extracted_fields, L"DATE OF PRODUCTION", std::vector<std::wstring>{date_of_production});
							fill_field(extracted_fields, L"ITEMS", items);
							fill_field(extracted_fields, L"UNIT", units);
							fill_field(extracted_fields, L"RESULT", results);
						}
						else if (class_name == L"POWERTECH") {
							auto product_name = extract_product_name(configuration, class_name, searched_fields, blocks, image_size);
							auto lot_no_pair = extract_lot_no(configuration, class_name, searched_fields, blocks, image_size);
							auto date_pair = extract_date_of_production(configuration, class_name, searched_fields, blocks, image_size);
							auto lot_item_map = extract_items_unit_results(configuration, class_name, searched_fields, blocks, image_size, lot_no_pair, date_pair);

							std::vector<std::wstring> lot_no;
							std::transform(lot_no_pair.begin(), lot_no_pair.end(), std::back_inserter(lot_no), [](std::pair<cv::Rect, std::wstring>& pair) {
								return pair.second;
							});

							std::vector<std::wstring> date_of_production;
							std::transform(date_pair.begin(), date_pair.end(), std::back_inserter(date_of_production), [](std::pair<cv::Rect, std::wstring>& pair) {
								return pair.second;
							});

							std::vector<std::wstring> items;
							std::vector<std::wstring> units;
							std::vector<std::wstring> results;
							transform_list(lot_item_map, items, units, results);

							fill_field(extracted_fields, L"COMPANY NAME", std::vector<std::wstring>{L"주식회사 파워텍"});
							fill_field(extracted_fields, L"PRODUCT NAME", product_name);
							fill_field(extracted_fields, L"LOT NO", lot_no);
							fill_field(extracted_fields, L"DATE OF PRODUCTION", std::vector<std::wstring>{date_of_production});
							fill_field(extracted_fields, L"ITEMS", items);
							fill_field(extracted_fields, L"UNIT", units);
							fill_field(extracted_fields, L"RESULT", results);
						}
						else if (class_name == L"DONGSUNG") {
							//auto company_name = extract_company_name(configuration, class_name, searched_fields, blocks, image_size);
							auto product_name = extract_product_name(configuration, class_name, searched_fields, blocks, image_size);
							auto lot_no_pair = extract_lot_no(configuration, class_name, searched_fields, blocks, image_size);
							auto date_pair = extract_date_of_production(configuration, class_name, searched_fields, blocks, image_size);
							auto lot_item_map = extract_items_unit_results(configuration, class_name, searched_fields, blocks, image_size, lot_no_pair, date_pair);

							std::vector<std::wstring> lot_no;
							std::transform(lot_no_pair.begin(), lot_no_pair.end(), std::back_inserter(lot_no), [](std::pair<cv::Rect, std::wstring>& pair) {
								return pair.second;
							});

							std::vector<std::wstring> date_of_production;
							std::transform(date_pair.begin(), date_pair.end(), std::back_inserter(date_of_production), [](std::pair<cv::Rect, std::wstring>& pair) {
								return pair.second;
							});

							std::vector<std::wstring> items;
							std::vector<std::wstring> units;
							std::vector<std::wstring> results;
							transform_list(lot_item_map, items, units, results);

							fill_field(extracted_fields, L"COMPANY NAME", std::vector<std::wstring>{L"(주) 동성코퍼레이션"});
							fill_field(extracted_fields, L"PRODUCT NAME", product_name);
							fill_field(extracted_fields, L"LOT NO", lot_no);
							fill_field(extracted_fields, L"DATE OF PRODUCTION", std::vector<std::wstring>{date_of_production});
							fill_field(extracted_fields, L"ITEMS", items);
							fill_field(extracted_fields, L"UNIT", units);
							fill_field(extracted_fields, L"RESULT", results);
						}
						else if (class_name == L"TEASUNG CHEMICAL") {
							//auto company_name = extract_company_name(configuration, class_name, searched_fields, blocks, image_size);
							auto product_name = extract_product_name(configuration, class_name, searched_fields, blocks, image_size);
							auto lot_no_pair = extract_lot_no(configuration, class_name, searched_fields, blocks, image_size);
							auto date_pair = extract_date_of_production(configuration, class_name, searched_fields, blocks, image_size);
							auto lot_item_map = extract_items_unit_results(configuration, class_name, searched_fields, blocks, image_size, lot_no_pair, date_pair);

							std::vector<std::wstring> lot_no;
							std::transform(lot_no_pair.begin(), lot_no_pair.end(), std::back_inserter(lot_no), [](std::pair<cv::Rect, std::wstring>& pair) {
								return pair.second;
							});

							std::vector<std::wstring> date_of_production;
							std::transform(date_pair.begin(), date_pair.end(), std::back_inserter(date_of_production), [](std::pair<cv::Rect, std::wstring>& pair) {
								return pair.second;
							});

							std::vector<std::wstring> items;
							std::vector<std::wstring> units;
							std::vector<std::wstring> results;
							transform_list(lot_item_map, items, units, results);

							fill_field(extracted_fields, L"COMPANY NAME", std::vector<std::wstring>{L"TAE SUNG CHEMICHAL. CO.LTD"});
							fill_field(extracted_fields, L"PRODUCT NAME", product_name);
							fill_field(extracted_fields, L"LOT NO", lot_no);
							fill_field(extracted_fields, L"DATE OF PRODUCTION", std::vector<std::wstring>{date_of_production});
							fill_field(extracted_fields, L"ITEMS", items);
							fill_field(extracted_fields, L"UNIT", units);
							fill_field(extracted_fields, L"RESULT", results);
						}
						else if (class_name == L"UNID") {
							auto product_name = extract_product_name(configuration, class_name, searched_fields, blocks, image_size);
							auto lot_no_pair = extract_lot_no(configuration, class_name, searched_fields, blocks, image_size);
							std::vector<std::pair<cv::Rect, std::wstring>> trash_date_pair;
							auto lot_item_map = extract_items_unit_results(configuration, class_name, searched_fields, blocks, image_size, lot_no_pair, trash_date_pair);

							std::vector<std::wstring> lot_no;
							std::transform(lot_no_pair.begin(), lot_no_pair.end(), std::back_inserter(lot_no), [](std::pair<cv::Rect, std::wstring>& pair) {
								return pair.second;
							});

							std::vector<std::wstring> items;
							std::vector<std::wstring> units;
							std::vector<std::wstring> results;
							transform_list(lot_item_map, items, units, results);

							fill_field(extracted_fields, L"COMPANY NAME", std::vector<std::wstring>{L"(주)유니드"});
							fill_field(extracted_fields, L"PRODUCT NAME", product_name);
							fill_field(extracted_fields, L"LOT NO", lot_no);
							fill_field(extracted_fields, L"DATE OF PRODUCTION", std::vector<std::wstring>{L""});
							fill_field(extracted_fields, L"ITEMS", items);
							fill_field(extracted_fields, L"UNIT", units);
							fill_field(extracted_fields, L"RESULT", results);
						}
						else if (class_name == L"BASF_B") {
							auto product_name = extract_product_name(configuration, class_name, searched_fields, blocks, image_size);
							auto lot_no_pair = extract_lot_no(configuration, class_name, searched_fields, blocks, image_size);
							std::vector<std::pair<cv::Rect, std::wstring>> trash_date_pair;
							auto lot_item_map = extract_items_unit_results(configuration, class_name, searched_fields, blocks, image_size, lot_no_pair, trash_date_pair);

							std::vector<std::wstring> lot_no;
							std::transform(lot_no_pair.begin(), lot_no_pair.end(), std::back_inserter(lot_no), [](std::pair<cv::Rect, std::wstring>& pair) {
								return pair.second;
							});

							std::vector<std::wstring> items;
							std::vector<std::wstring> units;
							std::vector<std::wstring> results;
							transform_list(lot_item_map, items, units, results);


							fill_field(extracted_fields, L"COMPANY NAME", std::vector<std::wstring>{L"BASF Company Ltd."});
							fill_field(extracted_fields, L"PRODUCT NAME", product_name);
							fill_field(extracted_fields, L"LOT NO", lot_no);
							fill_field(extracted_fields, L"DATE OF PRODUCTION", std::vector<std::wstring>{L""});
							fill_field(extracted_fields, L"ITEMS", items);
							fill_field(extracted_fields, L"UNIT", units);
							fill_field(extracted_fields, L"RESULT", results);
						}
						else if (class_name == L"SUNGU") {
							//auto company_name = extract_company_name(configuration, class_name, searched_fields, blocks, image_size);
							auto product_name = extract_product_name(configuration, class_name, searched_fields, blocks, image_size);
							auto lot_no_pair = extract_lot_no(configuration, class_name, searched_fields, blocks, image_size);
							auto date_pair = extract_date_of_production(configuration, class_name, searched_fields, blocks, image_size);
							auto lot_item_map = extract_items_unit_results(configuration, class_name, searched_fields, blocks, image_size, lot_no_pair, date_pair);

							std::vector<std::wstring> lot_no;
							std::transform(lot_no_pair.begin(), lot_no_pair.end(), std::back_inserter(lot_no), [](std::pair<cv::Rect, std::wstring>& pair) {
								return pair.second;
							});

							std::vector<std::wstring> date_of_production;
							std::transform(date_pair.begin(), date_pair.end(), std::back_inserter(date_of_production), [](std::pair<cv::Rect, std::wstring>& pair) {
								return pair.second;
							});

							std::vector<std::wstring> items;
							std::vector<std::wstring> units;
							std::vector<std::wstring> results;
							transform_list(lot_item_map, items, units, results);

							fill_field(extracted_fields, L"COMPANY NAME", std::vector<std::wstring>{L"(주) 선구"});
							fill_field(extracted_fields, L"PRODUCT NAME", product_name);
							fill_field(extracted_fields, L"LOT NO", lot_no);
							fill_field(extracted_fields, L"DATE OF PRODUCTION", std::vector<std::wstring>{date_of_production});
							fill_field(extracted_fields, L"ITEMS", items);
							fill_field(extracted_fields, L"UNIT", units);
							fill_field(extracted_fields, L"RESULT", results);
						}
						else if (class_name == L"LANXESS") {
							//auto company_name = extract_company_name(configuration, class_name, searched_fields, blocks, image_size);
							auto product_name = extract_product_name(configuration, class_name, searched_fields, blocks, image_size);
							auto lot_no_pair = extract_lot_no(configuration, class_name, searched_fields, blocks, image_size);
							auto date_pair = extract_date_of_production(configuration, class_name, searched_fields, blocks, image_size);
							auto lot_item_map = extract_items_unit_results(configuration, class_name, searched_fields, blocks, image_size, lot_no_pair, date_pair);

							std::vector<std::wstring> lot_no;
							std::transform(lot_no_pair.begin(), lot_no_pair.end(), std::back_inserter(lot_no), [](std::pair<cv::Rect, std::wstring>& pair) {
								return pair.second;
							});

							std::vector<std::wstring> date_of_production;
							std::transform(date_pair.begin(), date_pair.end(), std::back_inserter(date_of_production), [](std::pair<cv::Rect, std::wstring>& pair) {
								return pair.second;
							});

							std::vector<std::wstring> items;
							std::vector<std::wstring> units;
							std::vector<std::wstring> results;
							transform_list(lot_item_map, items, units, results);

							fill_field(extracted_fields, L"COMPANY NAME", std::vector<std::wstring>{L"LANXESS Solutions US Inc."});
							fill_field(extracted_fields, L"PRODUCT NAME", product_name);
							fill_field(extracted_fields, L"LOT NO", lot_no);
							fill_field(extracted_fields, L"DATE OF PRODUCTION", std::vector<std::wstring>{date_of_production});
							fill_field(extracted_fields, L"ITEMS", items);
							fill_field(extracted_fields, L"UNIT", units);
							fill_field(extracted_fields, L"RESULT", results);
						}
						else if (class_name == L"SKGC") {
							//auto company_name = extract_company_name(configuration, class_name, searched_fields, blocks, image_size);
							auto product_name = extract_product_name(configuration, class_name, searched_fields, blocks, image_size);
							auto lot_no_pair = extract_lot_no(configuration, class_name, searched_fields, blocks, image_size);
							auto date_pair = extract_date_of_production(configuration, class_name, searched_fields, blocks, image_size);
							auto lot_item_map = extract_items_unit_results(configuration, class_name, searched_fields, blocks, image_size, lot_no_pair, date_pair);

							std::vector<std::wstring> lot_no;
							std::transform(lot_no_pair.begin(), lot_no_pair.end(), std::back_inserter(lot_no), [](std::pair<cv::Rect, std::wstring>& pair) {
								return pair.second;
							});

							std::vector<std::wstring> date_of_production;
							std::transform(date_pair.begin(), date_pair.end(), std::back_inserter(date_of_production), [](std::pair<cv::Rect, std::wstring>& pair) {
								return pair.second;
							});

							std::vector<std::wstring> items;
							std::vector<std::wstring> units;
							std::vector<std::wstring> results;
							transform_list(lot_item_map, items, units, results);

							fill_field(extracted_fields, L"COMPANY NAME", std::vector<std::wstring>{L"SK global chemical"});
							fill_field(extracted_fields, L"PRODUCT NAME", product_name);
							fill_field(extracted_fields, L"LOT NO", lot_no);
							fill_field(extracted_fields, L"DATE OF PRODUCTION", std::vector<std::wstring>{date_of_production});
							fill_field(extracted_fields, L"ITEMS", items);
							fill_field(extracted_fields, L"UNIT", units);
							fill_field(extracted_fields, L"RESULT", results);
						}
						else if (class_name == L"XIAMETER") {
							//auto company_name = extract_company_name(configuration, class_name, searched_fields, blocks, image_size);
							auto product_name = extract_product_name(configuration, class_name, searched_fields, blocks, image_size);
							auto lot_no_pair = extract_lot_no(configuration, class_name, searched_fields, blocks, image_size);
							auto date_pair = extract_date_of_production(configuration, class_name, searched_fields, blocks, image_size);
							auto lot_item_map = extract_items_unit_results(configuration, class_name, searched_fields, blocks, image_size, lot_no_pair, date_pair);

							std::vector<std::wstring> lot_no;
							std::transform(lot_no_pair.begin(), lot_no_pair.end(), std::back_inserter(lot_no), [](std::pair<cv::Rect, std::wstring>& pair) {
								return pair.second;
							});

							std::vector<std::wstring> date_of_production;
							std::transform(date_pair.begin(), date_pair.end(), std::back_inserter(date_of_production), [](std::pair<cv::Rect, std::wstring>& pair) {
								return pair.second;
							});

							std::vector<std::wstring> items;
							std::vector<std::wstring> units;
							std::vector<std::wstring> results;
							transform_list(lot_item_map, items, units, results);

							fill_field(extracted_fields, L"COMPANY NAME", std::vector<std::wstring>{L"한국다우코닝 (주)"});
							fill_field(extracted_fields, L"PRODUCT NAME", product_name);
							fill_field(extracted_fields, L"LOT NO", lot_no);
							fill_field(extracted_fields, L"DATE OF PRODUCTION", std::vector<std::wstring>{date_of_production});
							fill_field(extracted_fields, L"ITEMS", items);
							fill_field(extracted_fields, L"UNIT", units);
							fill_field(extracted_fields, L"RESULT", results);
						}
						else if (class_name == L"DUKSAN") {
							auto product_name = extract_product_name(configuration, class_name, searched_fields, blocks, image_size);
							auto lot_no_pair = extract_lot_no(configuration, class_name, searched_fields, blocks, image_size);
							auto date_pair = extract_date_of_production(configuration, class_name, searched_fields, blocks, image_size);
							auto lot_item_map = extract_items_unit_results(configuration, class_name, searched_fields, blocks, image_size, lot_no_pair, date_pair);

							std::vector<std::wstring> lot_no;
							std::transform(lot_no_pair.begin(), lot_no_pair.end(), std::back_inserter(lot_no), [](std::pair<cv::Rect, std::wstring>& pair) {
								return pair.second;
							});

							std::vector<std::wstring> date_of_production;
							std::transform(date_pair.begin(), date_pair.end(), std::back_inserter(date_of_production), [](std::pair<cv::Rect, std::wstring>& pair) {
								return pair.second;
							});

							std::vector<std::wstring> items;
							std::vector<std::wstring> units;
							std::vector<std::wstring> results;
							transform_list(lot_item_map, items, units, results);

							fill_field(extracted_fields, L"COMPANY NAME", std::vector<std::wstring>{L"DUKSAN PURE CHEMICALS CO.LTD"});
							fill_field(extracted_fields, L"PRODUCT NAME", product_name);
							fill_field(extracted_fields, L"LOT NO", lot_no);
							fill_field(extracted_fields, L"DATE OF PRODUCTION", std::vector<std::wstring>{date_of_production});
							fill_field(extracted_fields, L"ITEMS", items);
							fill_field(extracted_fields, L"UNIT", units);
							fill_field(extracted_fields, L"RESULT", results);
						}
						else if (class_name == L"ALBEMARLE") {
							auto product_name = extract_product_name(configuration, class_name, searched_fields, blocks, image_size);
							auto lot_no_pair = extract_lot_no(configuration, class_name, searched_fields, blocks, image_size);
							auto lot_item_map = extract_items_unit_results(configuration, class_name, searched_fields, blocks, image_size, lot_no_pair, std::vector<std::pair<cv::Rect, std::wstring>>());

							std::vector<std::wstring> lot_no;
							std::transform(lot_no_pair.begin(), lot_no_pair.end(), std::back_inserter(lot_no), [](std::pair<cv::Rect, std::wstring>& pair) {
								return pair.second;
							});

							std::vector<std::wstring> items;
							std::vector<std::wstring> units;
							std::vector<std::wstring> results;
							transform_list(lot_item_map, items, units, results);

							fill_field(extracted_fields, L"COMPANY NAME", std::vector<std::wstring>{L"ALBEMARLE CORPORATION"});
							fill_field(extracted_fields, L"PRODUCT NAME", product_name);
							fill_field(extracted_fields, L"LOT NO", lot_no);
							fill_field(extracted_fields, L"DATE OF PRODUCTION", std::vector<std::wstring>{L""});
							fill_field(extracted_fields, L"ITEMS", items);
							fill_field(extracted_fields, L"UNIT", units);
							fill_field(extracted_fields, L"RESULT", results);
						}
						else if (class_name == L"NOF") {
							auto product_name = extract_product_name(configuration, class_name, searched_fields, blocks, image_size);
							auto lot_no_pair = extract_lot_no(configuration, class_name, searched_fields, blocks, image_size);
							auto lot_item_map = extract_items_unit_results_using_log_no_pair(configuration, class_name, searched_fields, blocks, image_size, lot_no_pair);

							std::vector<std::wstring> lot_no;
							std::transform(lot_no_pair.begin(), lot_no_pair.end(), std::back_inserter(lot_no), [](std::pair<cv::Rect, std::wstring>& pair) {
								return pair.second;
							});

							std::vector<std::wstring> items;
							std::vector<std::wstring> units;
							std::vector<std::wstring> results;
							transform_list(lot_item_map, items, units, results);

							fill_field(extracted_fields, L"COMPANY NAME", std::vector<std::wstring>{L"NOF CORPORATION"});
							fill_field(extracted_fields, L"PRODUCT NAME", product_name);
							fill_field(extracted_fields, L"LOT NO", lot_no);
							fill_field(extracted_fields, L"DATE OF PRODUCTION", std::vector<std::wstring>{L""});
							fill_field(extracted_fields, L"ITEMS", items);
							fill_field(extracted_fields, L"UNIT", units);
							fill_field(extracted_fields, L"RESULT", results);
						}
						else if (class_name == L"CHIMEI") {
							auto product_name = extract_product_name(configuration, class_name, searched_fields, blocks, image_size);
							auto lot_no_pair = extract_lot_no(configuration, class_name, searched_fields, blocks, image_size);
							auto lot_item_map = extract_items_unit_results_using_log_no_pair(configuration, class_name, searched_fields, blocks, image_size, lot_no_pair);

							std::vector<std::wstring> lot_no;
							std::transform(lot_no_pair.begin(), lot_no_pair.end(), std::back_inserter(lot_no), [](std::pair<cv::Rect, std::wstring>& pair) {
								return pair.second;
							});

							std::vector<std::wstring> items;
							std::vector<std::wstring> units;
							std::vector<std::wstring> results;
							transform_list(lot_item_map, items, units, results);

							fill_field(extracted_fields, L"COMPANY NAME", std::vector<std::wstring>{L"CHIMEI CORPORATION"});
							fill_field(extracted_fields, L"PRODUCT NAME", product_name);
							fill_field(extracted_fields, L"LOT NO", lot_no);
							fill_field(extracted_fields, L"DATE OF PRODUCTION", std::vector<std::wstring>{L""});
							fill_field(extracted_fields, L"ITEMS", items);
							fill_field(extracted_fields, L"UNIT", units);
							fill_field(extracted_fields, L"RESULT", results);
						}
						else {
							// auto company_name = extract_company_name(configuration, class_name, searched_fields, blocks, image_size);
							auto product_name = extract_product_name(configuration, class_name, searched_fields, blocks, image_size);
							auto lot_no_pair = extract_lot_no(configuration, class_name, searched_fields, blocks, image_size);
							auto date_pair = extract_date_of_production(configuration, class_name, searched_fields, blocks, image_size);
							auto lot_item_map = extract_items_unit_results(configuration, class_name, searched_fields, blocks, image_size, lot_no_pair, date_pair);

							std::vector<std::wstring> lot_no;
							std::transform(lot_no_pair.begin(), lot_no_pair.end(), std::back_inserter(lot_no), [](std::pair<cv::Rect, std::wstring>& pair) {
								return pair.second;
							});

							std::vector<std::wstring> date_of_production;
							std::transform(date_pair.begin(), date_pair.end(), std::back_inserter(date_of_production), [](std::pair<cv::Rect, std::wstring>& pair) {
								return pair.second;
							});

							std::vector<std::wstring> items;
							std::vector<std::wstring> units;
							std::vector<std::wstring> results;
							transform_list(lot_item_map, items, units, results);

							fill_field(extracted_fields, L"COMPANY NAME", std::vector<std::wstring>{L"OTHERS"});
							fill_field(extracted_fields, L"PRODUCT NAME", product_name);
							fill_field(extracted_fields, L"LOT NO", lot_no);
							fill_field(extracted_fields, L"DATE OF PRODUCTION", std::vector<std::wstring>{date_of_production});
							fill_field(extracted_fields, L"ITEMS", items);
							fill_field(extracted_fields, L"UNIT", units);
							fill_field(extracted_fields, L"RESULT", results);
						}



#ifdef LOG_USE_WOFSTREAM            // 문서단위 결과
						const auto text = to_wstring(blocks);
						txt_file << L"ALL : " << std::endl;
						txt_file << text << std::endl << std::endl;

						txt_file << L"- 파일 : " << files[i] << std::endl;
						txt_file << L"- 분류 : " << class_name << std::endl;

						txt_file << "- " << L"업체명" << " : " << boost::algorithm::join(get_field_values(extracted_fields, L"COMPANY NAME"), L",") << std::endl;
						txt_file << "- " << L"제품명" << " : " << boost::algorithm::join(get_field_values(extracted_fields, L"PRODUCT NAME"), L",") << std::endl;
						txt_file << "- " << L"Lot 번호" << " : " << boost::algorithm::join(get_field_values(extracted_fields, L"LOT NO"), L",") << std::endl;
						txt_file << "- " << L"생산일" << " : " << boost::algorithm::join(get_field_values(extracted_fields, L"DATE OF PRODUCTION"), L",") << std::endl;
						txt_file << "- " << L"평가항목" << " : " << boost::algorithm::join(get_field_values(extracted_fields, L"ITEMS"), L",") << std::endl;
						txt_file << "- " << L"단위" << " : " << boost::algorithm::join(get_field_values(extracted_fields, L"UNIT"), L",") << std::endl;
						txt_file << "- " << L"평가결과" << " : " << boost::algorithm::join(get_field_values(extracted_fields, L"RESULT"), L",") << std::endl;
						txt_file.close();
#endif

						if (extracted_fields.empty())
							continue;

						if (fields.find(class_name) == fields.end())
							fields.emplace(class_name, std::vector<std::unordered_map<std::wstring, std::vector<std::wstring>>>());

						fields.at(class_name).emplace_back(extracted_fields);
					}

#if __USE_ENGINE__
					release_engine_object(std::make_pair(loader, engine));
#endif
					// } , std::stoi(configuration.at(L"engine").at(L"concurrency")));
				}
				catch (_com_error& e) {
					spdlog::get("recognizer")->error("exception : {} : ({} : {})", to_cp949(e.Description().GetBSTR()), __FILE__, __LINE__);
				}

				return extracted_fields;
			}

			std::unordered_map<std::wstring, std::vector<std::wstring>>
				recognize(const boost::filesystem::path& path,
				const std::wstring& class_name) override
			{
				throw std::logic_error("not implmented");
			}

		private:
			static std::vector<std::wstring>
				get_field_values(const std::unordered_map<std::wstring, std::vector<std::wstring>>& extracted_fields, const std::wstring& field_column) {
				for (const auto& field : extracted_fields) {
					if (std::get<0>(field) == field_column) {
						return std::get<1>(field);
					}
				}

				return std::vector<std::wstring>();
			}

			static std::wstring
				extract_company_name(const configuration& configuration, const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
				const std::vector<block>& blocks, const cv::Size& image_size)
			{
				if (fields.find(L"COMPANY NAME") == fields.end())
					return L"";

				std::vector<std::pair<cv::Rect, std::wstring>> company_name;

				if (category == L"LG HNH") {
					company_name = extract_field_values(fields.at(L"COMPANY NAME"), blocks,
						search_self,
						default_preprocess,
						default_extract,
						postprocess_uppercase);



					if (company_name.empty()) {
						company_name = extract_field_values(fields.at(L"COMPANY NAME"), blocks,
							std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
							default_preprocess,
							default_extract,
							postprocess_uppercase);
					}
				}

				if (company_name.empty()) {
					return L"";
				}


				//wprintf(L"extract company_name file = %s\n", std::get<1>(company_name[0]).c_str());


				return std::get<1>(company_name[0]);
			}

			static std::pair<cv::Rect, std::wstring>
				preprocess_product_name(const std::pair<cv::Rect, std::wstring>& product_name)
			{
				auto cleaned = boost::regex_replace(std::get<1>(product_name), boost::wregex(L"(Cust)(.*)"), L"");
				cleaned = boost::regex_replace(cleaned, boost::wregex(L"[:;] ?"), L"");

				return std::make_pair(std::get<0>(product_name), cleaned);
			}

			static void filter_fields(std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>& product_fields,
									  std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>& sub_product_fields) {

				if (sub_product_fields.size() <= 0) {
					return;
				}

				boost::remove_erase_if(product_fields, [&sub_product_fields](const std::tuple<cv::Rect, std::wstring, cv::Range>& fields) {
					cv::Rect rect = std::get<0>(fields);
					rect.y = 0;
					rect.height = std::numeric_limits<int>::max();
					for (auto& sub_field : sub_product_fields) {
						const auto& coll = std::get<0>(sub_field) & rect;
						if (coll.width > 0) {
							return false;
						}
					}
					return true;
				});
			}

			static std::vector<std::wstring>
				extract_product_name(const configuration& configuration, const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
				const std::vector<block>& blocks, const cv::Size& image_size)
			{
				if (fields.find(L"PRODUCT NAME") == fields.end())
					return std::vector<std::wstring>();

				std::vector<std::pair<cv::Rect, std::wstring>> product_name;
				std::vector<std::wstring> extracted_product_name;

				if (category == L"LG HNH") {
					product_name = extract_field_values(fields.at(L"PRODUCT NAME"), blocks,
						std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						default_preprocess,
						default_extract,
						std::bind(&coa_document_recognizer::postprocess_product_name, std::placeholders::_1, category));

					if (product_name.empty()) {
						product_name = extract_field_values(fields.at(L"PRODUCT NAME"), blocks,
							search_self,
							default_preprocess,
							default_extract,
							std::bind(&coa_document_recognizer::postprocess_product_name, std::placeholders::_1, category));
					}
				}
				else if (category == L"POWERTECH") {
					product_name = extract_field_values(fields.at(L"PRODUCT NAME"), blocks,
						search_self,
						std::bind(&coa_document_recognizer::preprocess_product_name, std::placeholders::_1),
						default_extract,
						std::bind(&coa_document_recognizer::postprocess_product_name, std::placeholders::_1, category));

					if (product_name.empty()) {
						product_name = extract_field_values(fields.at(L"PRODUCT NAME"), blocks,
							std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
							std::bind(&coa_document_recognizer::preprocess_product_name, std::placeholders::_1),
							default_extract,
							std::bind(&coa_document_recognizer::postprocess_product_name, std::placeholders::_1, category));
					}
				}
				else if (category == L"BASF") {
					product_name = extract_field_values(fields.at(L"PRODUCT NAME"), blocks,
						std::bind(find_nearest_down_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false, false, false),
						default_preprocess,
						default_extract,
						default_postprocess);
				}
				else if (category == L"DONGSUNG") {
					product_name = extract_field_values(fields.at(L"PRODUCT NAME"), blocks,
						std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						default_preprocess,
						default_extract,
						std::bind(&coa_document_recognizer::postprocess_product_name, std::placeholders::_1, category));

					if (product_name.empty()) {
						product_name = extract_field_values(fields.at(L"PRODUCT NAME"), blocks,
							search_self,
							default_preprocess,
							default_extract,
							std::bind(&coa_document_recognizer::postprocess_product_name, std::placeholders::_1, category));
					}

				}
				else if (category == L"TEASUNG CHEMICAL") {
					product_name = extract_field_values(fields.at(L"PRODUCT NAME"), blocks,
						search_self,
						default_preprocess,
						default_extract,
						std::bind(&coa_document_recognizer::postprocess_product_name, std::placeholders::_1, category));
				}
				else if (category == L"UNID") {
					product_name = extract_field_values(fields.at(L"PRODUCT NAME"), blocks,
						search_self,
						std::bind(&coa_document_recognizer::preprocess_product_name, std::placeholders::_1),
						default_extract,
						std::bind(&coa_document_recognizer::postprocess_product_name, std::placeholders::_1, category));

					if (product_name.empty()) {
						product_name = extract_field_values(fields.at(L"PRODUCT NAME"), blocks,
							std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
							std::bind(&coa_document_recognizer::preprocess_product_name, std::placeholders::_1),
							default_extract,
							std::bind(&coa_document_recognizer::postprocess_product_name, std::placeholders::_1, category));
					}
				}
				else if (category == L"BASF_B") {
					product_name = extract_field_values(fields.at(L"PRODUCT NAME"), blocks,
						search_self,
						std::bind(&coa_document_recognizer::preprocess_product_name, std::placeholders::_1),
						default_extract,
						std::bind(&coa_document_recognizer::postprocess_product_name, std::placeholders::_1, category));

					if (product_name.empty()) {
						product_name = extract_field_values(fields.at(L"PRODUCT NAME"), blocks,
							std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
							std::bind(&coa_document_recognizer::preprocess_product_name, std::placeholders::_1),
							default_extract,
							std::bind(&coa_document_recognizer::postprocess_product_name, std::placeholders::_1, category));
					}
				}
				else if (category == L"SUNGU"){
					product_name = extract_field_values(fields.at(L"PRODUCT NAME"), blocks,
						std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						default_preprocess,
						default_extract,
						default_postprocess);
					if (product_name.empty() && fields.find(L"SUB_PRODUCT_NAME") != fields.end()) {
						product_name = extract_field_values(fields.at(L"SUB_PRODUCT_NAME"), blocks,
							std::bind(find_nearest_down_lines, std::placeholders::_1, std::placeholders::_2, 0.01, 0.25, 2.5, false, false, false),
							default_preprocess,
							default_extract,
							default_postprocess);
						std::sort(product_name.begin(), product_name.end(), [](std::pair<cv::Rect, std::wstring>& a, std::pair<cv::Rect, std::wstring>& b) {
							return std::get<0>(a).x < std::get<0>(b).x;
						});
					}
				}
				else if (category == L"LANXESS"){
					product_name = extract_field_values(fields.at(L"PRODUCT NAME"), blocks,
						std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						default_preprocess,
						default_extract,
						default_postprocess);
					if (product_name.empty()) {
						product_name = extract_field_values(fields.at(L"PRODUCT NAME"), blocks,
							search_self,
							default_preprocess,
							default_extract,
							default_postprocess);
					}
					if (product_name.empty()) {
						product_name = extract_field_values(fields.at(L"DATE OF PRODUCTION"), blocks,
							std::bind(find_nearest_up_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 3, false, false, false),
							default_preprocess,
							default_extract,
							default_postprocess);
					}

					std::sort(product_name.begin(), product_name.end(), [](const std::pair<cv::Rect, std::wstring>& a, const std::pair<cv::Rect, std::wstring>& b) {
						return a.second.length() > b.second.length();
					});

				}
				else if (category == L"SKGC") {
					product_name = extract_field_values(fields.at(L"PRODUCT NAME"), blocks,
						std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 3, false),
						default_preprocess,
						default_extract,
						default_postprocess);
				}
				else if (category == L"XIAMETER") {
					product_name = extract_field_values(fields.at(L"PRODUCT NAME"), blocks,
						std::bind(find_nearest_down_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 3, false, false, false),
						default_preprocess,
						default_extract,
						default_postprocess);
					if (product_name.empty() && fields.find(L"SUB_PRODUCT_NAME") != fields.end()) {
						product_name = extract_field_values(fields.at(L"SUB_PRODUCT_NAME"), blocks,
							std::bind(find_nearest_up_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 3, false, false, false),
							default_preprocess,
							default_extract,
							default_postprocess);
					}
				}
				else if (category == L"DUKSAN") {
					product_name = extract_field_values(fields.at(L"PRODUCT NAME"), blocks,
						std::bind(find_nearest_down_lines, std::placeholders::_1, std::placeholders::_2, 0.4, 0.0, 100, true, false, false),
						default_preprocess,
						default_extract,
						std::bind(&coa_document_recognizer::postprocess_product_name, std::placeholders::_1, category));
				}
				else if (category == L"WEIHAI") {
					product_name = extract_field_values(fields.at(L"PRODUCT NAME"), blocks,
						std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						default_preprocess,
						default_extract,
						default_postprocess);
					if (product_name.empty()) {
						product_name = extract_field_values(fields.at(L"PRODUCT NAME"), blocks,
							search_self,
							default_preprocess,
							default_extract,
							default_postprocess);
					}
				}
				else if (category == L"ALBEMARLE") {
					product_name = extract_field_values(fields.at(L"PRODUCT NAME"), blocks,
						std::bind(find_nearest_up_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false, false, false),
						default_preprocess,
						default_extract,
						std::bind(&coa_document_recognizer::postprocess_product_name, std::placeholders::_1, category));
				}
				else if (category == L"NOF") {
					auto product_fields = line_field_to_word_field(fields.at(L"PRODUCT NAME"), blocks);
					if (product_fields.empty() && fields.find(L"SUB_PRODUCT_NAME") != fields.end()) {
						auto product_pair = extract_field_values(fields.at(L"SUB_PRODUCT_NAME"), blocks,
							std::bind(find_nearest_up_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 3, false, false, false),
							default_preprocess,
							default_extract,
							std::bind(&coa_document_recognizer::postprocess_product_name, std::placeholders::_1, category));

						product_fields = pairs_to_fields(product_pair);
					}
					if (fields.find(L"SUB_PRODUCT_NAME") != fields.end()) {
						auto sub_product_fields = line_field_to_word_field(fields.at(L"SUB_PRODUCT_NAME"), blocks);
						filter_fields(product_fields, sub_product_fields);
					}
					product_name = extract_field_values(product_fields, blocks,
						std::bind(find_right_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						default_preprocess,
						default_extract,
						std::bind(&coa_document_recognizer::postprocess_product_name, std::placeholders::_1, category));
					if (product_fields.empty() && fields.find(L"SUB_PRODUCT_NAME") != fields.end()) {
						auto product_pair = extract_field_values(fields.at(L"SUB_PRODUCT_NAME"), blocks,
							std::bind(find_nearest_up_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 3, false, false, false),
							default_preprocess,
							default_extract,
							std::bind(&coa_document_recognizer::postprocess_product_name, std::placeholders::_1, category));
					}
				}
				else if (category == L"CHIMEI") {
					product_name = extract_field_values(fields.at(L"PRODUCT NAME"), blocks,
						std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						default_preprocess,
						default_extract,
						std::bind(&coa_document_recognizer::postprocess_product_name, std::placeholders::_1, category));
					if (product_name.empty()) {
						product_name = extract_field_values(fields.at(L"PRODUCT NAME"), blocks,
							search_self,
							default_preprocess,
							default_extract,
							std::bind(&coa_document_recognizer::postprocess_product_name, std::placeholders::_1, category));
					}
				}
				else {
					product_name = extract_field_values(fields.at(L"PRODUCT NAME"), blocks,
						std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						default_preprocess,
						default_extract,
						default_postprocess);
				}

				if (product_name.empty()) {
					return extracted_product_name;
				}

				if (product_name.size() > 0) {
					if (category == L"NOF") {
						std::vector<std::wstring> temp;
						std::transform(std::begin(product_name), std::end(product_name), std::back_inserter(temp), [](const std::pair<cv::Rect, std::wstring>& line) {
							return std::get<1>(line);
						});

						extracted_product_name.push_back(boost::algorithm::join(temp, L" "));
					}
					else {
						extracted_product_name.push_back(std::get<1>(product_name[0]));
					}

				}

				return extracted_product_name;
			}

			static std::wstring
				postprocess_product_name(const std::wstring& product_name, const std::wstring& category)
			{
				if (category == L"DONGSUNG") {
					if (product_name.length() < 3)
						return L"";

					auto cleaned = boost::regex_replace(product_name, boost::wregex(L"[:/']"), L"");
					// 역슬래시 제거 추가
					boost::trim(cleaned);
					return cleaned;
				}
				else if (category == L"TEASUNG CHEMICAL") {

					auto cleaned = boost::regex_replace(product_name, boost::wregex(L"[:]"), L"");
					boost::trim(cleaned);

					cleaned = boost::regex_replace(cleaned, boost::wregex(L"^니auide"), L"Liquide");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"^니ciude"), L"Liquide");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"^니guide"), L"Liquide");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"^Uquide"), L"Liquide");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"^Ljquhde"), L"Liquide");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"^Ljquide"), L"Liquide");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"^Uciuide"), L"Liquide");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"^니"), L"Li");
					// 역슬래시 제거 추가
					boost::trim(cleaned);
					return cleaned;
				}
				else if (category == L"BASF_B") {
					if (product_name.length() > 25)
						return L"";

					auto cleaned = boost::regex_replace(product_name, boost::wregex(L"-Butan(.*)"), L"-Butanediol");

					return cleaned;
				}
				else if (category == L"DUKSAN") {
					if (product_name.length() < 5)
						return L"";

					auto cleaned = boost::regex_replace(product_name, boost::wregex(L"phateTri"), L"phate tri");

					return cleaned;
				}
				else if (category == L"ALBEMARLE") {
					auto cleaned = boost::regex_replace(product_name, boost::wregex(L"TuB|IBB"), L"TBB");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"B3|3B|BS\\?|33"), L"BB");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"tbbpa"), L"TBBPA");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"phene?o?[\\?1]"), L"phenol");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L" ?- ?A\\.?"), L"-A");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"betrabromo"), L"tetrabromo");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"brornobi|brontobi|broniobi|broinobi|bromobx"), L"bromobi");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"T.etra|tevra"), L"tetra");

					return cleaned;
				}
				else if (category == L"LG HNH") {
					auto cleaned = boost::regex_replace(product_name, boost::wregex(L":"), L"");

					boost::trim(cleaned);

					return cleaned;
				}
				else {
					if (product_name.length() >= 25)
						return L"";

					auto cleaned = boost::regex_replace(product_name, boost::wregex(L":"), L"");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"-\\s"), L"$1");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"탄산칼[롭음]"), L"탄산칼륨");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"수산화칼[롭음]"), L"수산화칼륨");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"[)] ?([0-9]{2})%"), L") $1%");

					boost::trim(cleaned);

					return cleaned;
				}
			}
			static std::pair<cv::Rect, std::wstring>
				preprocess_max_hitting(const std::pair<cv::Rect, std::wstring>& input, const std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>&fields, const std::wstring category, const float hitting_threshold)
			{
				bool flag = false;
				for (const auto& field : fields) {
					int field_top = std::get<0>(field).y;
					int field_bottom = std::get<0>(field).br().y;
					int field_height = std::get<0>(field).height;

					int input_top = input.first.y;
					int input_bottom = input.first.br().y;
					int input_height = input.first.height;

					int coll_height = std::min(input_bottom, field_bottom) - std::max(field_top, input_top);

					if (coll_height > std::max(field_height, input_height) * hitting_threshold) {
						flag = true;
					}
				}

				if (!flag) {
					return std::make_pair(cv::Rect(), L"");
				}


				return input;
			}

			static std::pair<cv::Rect, std::wstring>
				preprocess_lot_no(const std::pair<cv::Rect, std::wstring>& lot_no)
			{
				auto cleaned = boost::regex_replace(std::get<1>(lot_no), boost::wregex(L"(Wei)(.*)"), L"");

				return std::make_pair(std::get<0>(lot_no), cleaned);
			}

			static std::vector<std::pair<cv::Rect, std::wstring>>
				extract_lot_no(const configuration& configuration, const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
				const std::vector<block>& blocks, const cv::Size& image_size)
			{
				std::vector<std::pair<cv::Rect, std::wstring>> lot_numbers;

				if (fields.find(L"LOT NO") == fields.end() && category != L"ALBEMARLE") {
					lot_numbers.emplace_back(std::make_pair(cv::Rect(), L""));

					return lot_numbers;
				}

				if (category == L"LG HNH" || category == L"BASF") {
					lot_numbers = extract_field_values(fields.at(L"LOT NO"), blocks,
						std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						default_preprocess,
						default_extract,
						std::bind(&coa_document_recognizer::postprocess_lot_no, std::placeholders::_1, category));

					if (lot_numbers.empty()) {
						lot_numbers = extract_field_values(fields.at(L"LOT NO"), blocks,
							search_self,
							default_preprocess,
							default_extract,
							std::bind(&coa_document_recognizer::postprocess_lot_no, std::placeholders::_1, category));
					}
				}
				else if (category == L"POWERTECH") {
					lot_numbers = extract_field_values(fields.at(L"LOT NO"), blocks,
						search_self,
						std::bind(&coa_document_recognizer::preprocess_lot_no, std::placeholders::_1),
						default_extract,
						std::bind(&coa_document_recognizer::postprocess_lot_no, std::placeholders::_1, category));

					if (lot_numbers.empty()) {
						lot_numbers = extract_field_values(fields.at(L"LOT NO"), blocks,
							std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
							std::bind(&coa_document_recognizer::preprocess_lot_no, std::placeholders::_1),
							default_extract,
							std::bind(&coa_document_recognizer::postprocess_lot_no, std::placeholders::_1, category));
					}
				}
				else if (category == L"DONGSUNG") {
					lot_numbers = extract_field_values(fields.at(L"LOT NO"), blocks,
						std::bind(find_right_lines, std::placeholders::_1, std::placeholders::_2, 0.7, 0.0, 100, false),
						default_preprocess,
						default_extract,
						default_postprocess);
				}
				else if (category == L"NOF") {
					lot_numbers = extract_field_values(fields.at(L"LOT NO"), blocks,
						std::bind(find_right_lines, std::placeholders::_1, std::placeholders::_2, 0.7, 0.0, 100, false),
						std::bind(&coa_document_recognizer::preprocess_max_hitting, std::placeholders::_1, fields.at(L"LOT NO"), category, 0.2),
						default_extract,
						default_postprocess);
				}
				else if (category == L"TEASUNG CHEMICAL") {
					lot_numbers = extract_field_values(fields.at(L"LOT NO"), blocks,
						std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						default_preprocess,
						default_extract,
						std::bind(&coa_document_recognizer::postprocess_lot_no, std::placeholders::_1, category));

					if (lot_numbers.empty()) {
						lot_numbers = extract_field_values(fields.at(L"LOT NO"), blocks,
							search_self,
							default_preprocess,
							default_extract,
							std::bind(&coa_document_recognizer::postprocess_lot_no, std::placeholders::_1, category));
					}
				}
				else if (category == L"UNID") {
					lot_numbers = extract_field_values(fields.at(L"LOT NO"), blocks,
						search_self,
						std::bind(&coa_document_recognizer::preprocess_lot_no, std::placeholders::_1),
						default_extract,
						std::bind(&coa_document_recognizer::postprocess_lot_no, std::placeholders::_1, category));

					if (lot_numbers.empty()) {
						lot_numbers = extract_field_values(fields.at(L"LOT NO"), blocks,
							std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
							std::bind(&coa_document_recognizer::preprocess_lot_no, std::placeholders::_1),
							default_extract,
							std::bind(&coa_document_recognizer::postprocess_lot_no, std::placeholders::_1, category));
					}
				}
				else if (category == L"BASF_B") {
					lot_numbers = extract_field_values(fields.at(L"LOT NO"), blocks,
						search_self,
						std::bind(&coa_document_recognizer::preprocess_lot_no, std::placeholders::_1),
						default_extract,
						std::bind(&coa_document_recognizer::postprocess_lot_no, std::placeholders::_1, category));

					if (lot_numbers.empty()) {
						lot_numbers = extract_field_values(fields.at(L"LOT NO"), blocks,
							std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
							std::bind(&coa_document_recognizer::preprocess_lot_no, std::placeholders::_1),
							default_extract,
							std::bind(&coa_document_recognizer::postprocess_lot_no, std::placeholders::_1, category));
					}

				}
				else if (category == L"SUNGU") {
					lot_numbers = extract_field_values(fields.at(L"LOT NO"), blocks,
						std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						default_preprocess,
						default_extract,
						default_postprocess);

					if (lot_numbers.empty()) {
						lot_numbers = extract_field_values(fields.at(L"LOT NO"), blocks,
							search_self,
							default_preprocess,
							default_extract,
							default_postprocess);
					}
				}
				else if (category == L"LANXESS") {
					lot_numbers = extract_field_values(fields.at(L"LOT NO"), blocks,
						search_self,
						default_preprocess,
						default_extract,
						std::bind(&coa_document_recognizer::postprocess_lot_no, std::placeholders::_1, category));

					if (lot_numbers.empty()) {
						lot_numbers = extract_field_values(fields.at(L"LOT NO"), blocks,
							std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
							default_preprocess,
							default_extract,
							default_postprocess);
					}
				}
				else if (category == L"SKGC") {
					lot_numbers = extract_field_values(fields.at(L"LOT NO"), blocks,
						std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						default_preprocess,
						default_extract,
						default_postprocess);

					if (lot_numbers.empty()) {
						lot_numbers = extract_field_values(fields.at(L"LOT NO"), blocks,
							search_self,
							default_preprocess,
							default_extract,
							default_postprocess);
					}
				}
				else if (category == L"XIAMETER") {
					auto lot_field = line_field_to_word_field(fields.at(L"LOT NO"), blocks);
					lot_numbers = extract_field_values(lot_field, blocks,
						std::bind(find_coa_nearest_down_words, std::placeholders::_1, std::placeholders::_2, 0.01, 0.25, 2.5, false, false, false, 1.5, false, 0),
						default_preprocess,
						default_extract,
						default_postprocess);

					if (lot_numbers.empty()) {
						lot_numbers = extract_field_values(fields.at(L"LOT NO"), blocks,
							search_self,
							default_preprocess,
							default_extract,
							default_postprocess);
					}
				}
				else if (category == L"DUKSAN") {
					auto lot_field = line_field_to_word_field(fields.at(L"LOT NO"), blocks);
					lot_numbers = extract_field_values(lot_field, blocks,
						std::bind(find_nearest_down_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false, false, false),
						default_preprocess,
						default_extract,
						std::bind(&coa_document_recognizer::postprocess_lot_no, std::placeholders::_1, category));
				}
				else if (category == L"WEIHAI") {
					lot_numbers = extract_field_values(fields.at(L"LOT NO"), blocks,
						std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						default_preprocess,
						default_extract,
						default_postprocess);

					if (lot_numbers.empty()) {
						lot_numbers = extract_field_values(fields.at(L"LOT NO"), blocks,
							search_self,
							default_preprocess,
							default_extract,
							default_postprocess);
					}
				}
				else if (category == L"ALBEMARLE") {
					if (fields.find(L"PRODUCT NAME") != fields.end()) {
						auto suggested_lot_numbers = extract_field_values(fields.at(L"PRODUCT NAME"), blocks,
							search_self,
							std::bind(&coa_document_recognizer::preprocess_lot_no, std::placeholders::_1),
							default_extract,
							std::bind(&coa_document_recognizer::postprocess_lot_no, std::placeholders::_1, category));

						if (suggested_lot_numbers.size() >= 1) {
							lot_numbers.emplace_back(suggested_lot_numbers[0]);
						}
					}
				}
				else if (category == L"CHIMEI") {
					int max_int = std::numeric_limits<int>::max();
					std::tuple<cv::Rect, std::wstring, cv::Range> top_lot_no_field
						= std::make_tuple(cv::Rect(max_int, max_int, max_int, max_int), L"", cv::Range(0, 0));
					for (auto field : fields.at(L"LOT NO")) {
						if (std::get<0>(field).y <= std::get<0>(top_lot_no_field).y) {
							top_lot_no_field = field;
						}
					}

					if (std::get<0>(top_lot_no_field).y < max_int) {
						std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>> suggested_lot_no_fields = { top_lot_no_field  };

						lot_numbers = extract_field_values(suggested_lot_no_fields, blocks,
							search_self,
							default_preprocess,
							default_extract,
							std::bind(&coa_document_recognizer::postprocess_lot_no, std::placeholders::_1, category));

						auto additional_lot_numbers = extract_field_values(suggested_lot_no_fields, blocks,
							std::bind(find_right_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
							default_preprocess,
							default_extract,
							std::bind(&coa_document_recognizer::postprocess_lot_no, std::placeholders::_1, category));

						for (auto additional_lot_number : additional_lot_numbers) {
							lot_numbers.emplace_back(additional_lot_number);
						}
					}
				}
				else {
					lot_numbers = extract_field_values(fields.at(L"LOT NO"), blocks,
						std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						default_preprocess,
						default_extract,
						default_postprocess);

					if (lot_numbers.empty()) {
						lot_numbers = extract_field_values(fields.at(L"LOT NO"), blocks,
							search_self,
							default_preprocess,
							default_extract,
							default_postprocess);
					}
				}

				if (lot_numbers.empty()) {
					lot_numbers.emplace_back(std::make_pair(cv::Rect(), L""));

					return lot_numbers;
				}

				if (category == L"LG HNH" || category == L"BASF") {
					lot_numbers.erase(lot_numbers.begin() + 1, lot_numbers.end());
				}

				return lot_numbers;
			}

			static std::wstring
				postprocess_lot_no(const std::wstring& lot_no, const std::wstring& category)
			{
				if (category == L"TEASUNG CHEMICAL") {
					auto cleaned = boost::regex_replace(lot_no, boost::wregex(L"이"), L"01");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"[:]"), L"");

					boost::trim(cleaned);

					return cleaned;
				}
				else if (category == L"BASF_B") {
					auto cleaned = boost::regex_replace(lot_no, boost::wregex(L"[a-z]"), L"");

					boost::trim(cleaned);

					return cleaned;
				}
				else if (category == L"DUKSAN") {
					auto cleaned = boost::algorithm::trim_copy(boost::to_upper_copy(lot_no));
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"LOT"), L"Lot");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"t~"), L"t-");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"(.*)Lot"), L"Lot");

					return cleaned;
				}
				else if (category == L"ALBEMARLE") {
					auto cleaned = boost::algorithm::trim_copy(boost::to_upper_copy(lot_no));
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"\\.? ?\\.?/ ?SAM(.*)"), L"");

					return cleaned;
				}
				else if (category == L"") {
					auto cleaned = boost::algorithm::trim_copy(boost::to_upper_copy(lot_no));
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"\\."), L"");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L" "), L"");

					return cleaned;
				}
				else {
					auto cleaned = boost::regex_replace(lot_no, boost::wregex(L":"), L"");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"\\."), L"");

					boost::trim(cleaned);

					return cleaned;
				}
			}

			static std::pair<cv::Rect, std::wstring>
				preprocess_date_of_production(const std::pair<cv::Rect, std::wstring>& date_of_production)
			{
				auto cleaned = boost::regex_replace(std::get<1>(date_of_production), boost::wregex(L"(Tes)(.*)"), L"");

				return std::make_pair(std::get<0>(date_of_production), cleaned);
			}

			static std::vector<std::pair<cv::Rect, std::wstring>>
				extract_date_of_production(const configuration& configuration, const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
				const std::vector<block>& blocks, const cv::Size& image_size)
			{
				if (fields.find(L"DATE OF PRODUCTION") == fields.end())
					return std::vector<std::pair<cv::Rect, std::wstring>>();

				std::vector<std::pair<cv::Rect, std::wstring>> dates;
				std::vector<std::pair<cv::Rect, std::wstring>> extracted_dates;

				if (category == L"LG HNH" || category == L"BASF") {
					dates = extract_field_values(fields.at(L"DATE OF PRODUCTION"), blocks,
						std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						default_preprocess,
						default_extract,
						default_postprocess);

					if (dates.empty()) {
						dates = extract_field_values(fields.at(L"DATE OF PRODUCTION"), blocks,
							search_self,
							default_preprocess,
							default_extract,
							default_postprocess);
					}
				}
				else if (category == L"POWERTECH") {
					dates = extract_field_values(fields.at(L"DATE OF PRODUCTION"), blocks,
						std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						std::bind(&coa_document_recognizer::preprocess_date_of_production, std::placeholders::_1),
						default_extract,
						std::bind(&coa_document_recognizer::postprocess_date_of_production, std::placeholders::_1));

					if (dates.empty()) {
						dates = extract_field_values(fields.at(L"DATE OF PRODUCTION"), blocks,
							search_self,
							std::bind(&coa_document_recognizer::preprocess_date_of_production, std::placeholders::_1),
							default_extract,
							std::bind(&coa_document_recognizer::postprocess_date_of_production, std::placeholders::_1));
					}
				}
				else if (category == L"DONGSUNG") {
					dates = extract_field_values(fields.at(L"DATE OF PRODUCTION"), blocks,
						std::bind(find_right_lines, std::placeholders::_1, std::placeholders::_2, 0.7, 0.0, 100, false),
						default_preprocess,
						default_extract,
						std::bind(&coa_document_recognizer::postprocess_date_of_production, std::placeholders::_1));
				}
				else if (category == L"TEASUNG CHEMICAL") {
					dates = extract_field_values(fields.at(L"DATE OF PRODUCTION"), blocks,
						std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						default_preprocess,
						default_extract,
						default_postprocess);
				}
				else if (category == L"UNID") {
					dates = extract_field_values(fields.at(L"DATE OF PRODUCTION"), blocks,
						std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						std::bind(&coa_document_recognizer::preprocess_date_of_production, std::placeholders::_1),
						default_extract,
						std::bind(&coa_document_recognizer::postprocess_date_of_production, std::placeholders::_1));

					if (dates.empty()) {
						dates = extract_field_values(fields.at(L"DATE OF PRODUCTION"), blocks,
							search_self,
							std::bind(&coa_document_recognizer::preprocess_date_of_production, std::placeholders::_1),
							default_extract,
							std::bind(&coa_document_recognizer::postprocess_date_of_production, std::placeholders::_1));
					}
				}
				else if (category == L"SUNGU") {
					dates = extract_field_values(fields.at(L"DATE OF PRODUCTION"), blocks,
						std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						default_preprocess,
						default_extract,
						postprocess_only_date);
					std::sort(dates.begin(), dates.end(), [](const std::pair<cv::Rect, std::wstring>& a, const std::pair<cv::Rect, std::wstring>& b){
						return a.second.length() > b.second.length();
					});
					if (dates.empty()) {
						dates = extract_field_values(std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>{
							std::make_tuple(cv::Rect(), L"", cv::Range())
						}, blocks,
						default_search,
						default_preprocess,
						create_coa_extract_function(L"[0-9]{3,4}[. ]{1,2}[0-9]{2}[. ]{1,2}[0-9]{2}"),
						postprocess_only_date);
					}
					std::sort(dates.begin(), dates.end(), [](const std::pair<cv::Rect, std::wstring>& a, const std::pair<cv::Rect, std::wstring>& b){
						return a.first.y > b.first.y;
					});
				}
				else if (category == L"LANXESS") {
					dates = extract_field_values(fields.at(L"DATE OF PRODUCTION"), blocks,
						search_self,
						default_preprocess,
						default_extract,
						std::bind(&coa_document_recognizer::postprocess_date_of_production, std::placeholders::_1));
					if (dates.empty()) {
						dates = extract_field_values(fields.at(L"DATE OF PRODUCTION"), blocks,
							std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
							default_preprocess,
							default_extract,
							std::bind(&coa_document_recognizer::postprocess_date_of_production, std::placeholders::_1));
					}
				}
				else if (category == L"XIAMETER") {
					auto date_field = line_field_to_word_field(fields.at(L"DATE OF PRODUCTION"), blocks);

					dates = extract_field_values(date_field, blocks,
						std::bind(find_coa_nearest_down_words, std::placeholders::_1, std::placeholders::_2, 0.01, 2, 2.5, false, false, false, 1.5, false, 0),
						default_preprocess,
						default_extract,
						std::bind(&coa_document_recognizer::postprocess_date_of_production, std::placeholders::_1));

					if (dates.empty()) {
						dates = extract_field_values(fields.at(L"DATE OF PRODUCTION"), blocks,
							search_self,
							default_preprocess,
							default_extract,
							std::bind(&coa_document_recognizer::postprocess_date_of_production, std::placeholders::_1));
					}
					if (dates.empty() && fields.find(L"SUB_DATE") != fields.end()) {
						auto sub_date_field = line_field_to_word_field(fields.at(L"SUB_DATE"), blocks);
						dates = extract_field_values(sub_date_field, blocks,
							std::bind(find_nearest_up_words, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 3, false, false, false),
							default_preprocess,
							default_extract,
							std::bind(&coa_document_recognizer::postprocess_date_of_production, std::placeholders::_1));
					}
				}
				else if (category == L"DUKSAN") {
					dates = extract_field_values(fields.at(L"DATE OF PRODUCTION"), blocks,
						std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						default_preprocess,
						default_extract,
						default_postprocess);

					if (dates.empty()) {
						dates = extract_field_values(fields.at(L"DATE OF PRODUCTION"), blocks,
							search_self,
							default_preprocess,
							default_extract,
							std::bind(&coa_document_recognizer::postprocess_date_of_production, std::placeholders::_1));
					}
				}
				else if (category == L"WEIHAI") {
					dates = extract_field_values(fields.at(L"DATE OF PRODUCTION"), blocks,
						std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						default_preprocess,
						default_extract,
						std::bind(&coa_document_recognizer::postprocess_date_of_production, std::placeholders::_1));
					if (dates.empty()) {
						dates = extract_field_values(fields.at(L"DATE OF PRODUCTION"), blocks,
							search_self,
							default_preprocess,
							default_extract,
							std::bind(&coa_document_recognizer::postprocess_date_of_production, std::placeholders::_1));
					}
				}
				else {
					dates = extract_field_values(fields.at(L"DATE OF PRODUCTION"), blocks,
						std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						default_preprocess,
						default_extract,
						default_postprocess);
				}

				if (dates.empty()) {
					return std::vector<std::pair<cv::Rect, std::wstring>>();
				}

				if (category == L"LG HNH" || category == L"BASF" || category == L"SUNGU") {
					dates.erase(dates.begin() + 1, dates.end());
				}

				for (auto a : dates) {
					extracted_dates.push_back(a);
					wprintf(L"extract date  = %s\n", std::get<1>(a).c_str());
				}

				return extracted_dates;
			}
			static std::wstring
				postprocess_remove_short(const std::wstring& lot_no, int threshold = 3) {
				return lot_no.length() > threshold ? lot_no : L"";
			}

			static std::wstring
				postprocess_date_of_production(const std::wstring& date_of_production)
			{
				if (date_of_production.length() < 3) {
					return L"";
				}

				auto cleaned = boost::regex_replace(date_of_production, boost::wregex(L"[:|'|•| ]"), L"");
				cleaned = boost::regex_replace(cleaned, boost::wregex(L"[Q|q]ua(.*)"), L"");
				cleaned = boost::regex_replace(cleaned, boost::wregex(L","), L".");
				cleaned = boost::regex_replace(cleaned, boost::wregex(L"이"), L"01");
				cleaned = boost::regex_replace(cleaned, boost::wregex(L"I1"), L"11");
				cleaned = boost::regex_replace(cleaned, boost::wregex(L"[(][)]"), L"0");
				cleaned = boost::regex_replace(cleaned, boost::wregex(L"L([0-9])"), L"1$1");

				return cleaned;
			}

			static bool isVerticalLine(const cv::Rect& rect_1, const cv::Rect& rect_2, float hitting_threshold) {
				auto basis_rect = rect_1;
				basis_rect.y = 0;
				basis_rect.height = std::numeric_limits<int>::max();

				const auto collision = basis_rect & rect_2;

				if (collision.width >= std::min(basis_rect.width, rect_2.width) * hitting_threshold) {
					return true;
				}
				return false;
			}

			static bool isHorizontalLine(const cv::Rect& rect_1, const cv::Rect& rect_2, float hitting_threshold) {
				auto basis_rect = rect_1;
				basis_rect.x = 0;
				basis_rect.width = std::numeric_limits<int>::max();

				const auto collision = basis_rect & rect_2;

				if (collision.height >= std::min(basis_rect.height, rect_2.height) * hitting_threshold) {
					return true;
				}
				return false;
			}

			static cv::Rect field_to_rect(const std::tuple<cv::Rect, std::wstring, cv::Range>& field) {
				cv::Rect ret = std::get<0>(field);
				cv::Range range = std::get<2>(field);
				int len = std::get<1>(field).length();
				if (len <= 0) {
					return ret;
				}
				int one_char_width = ret.width / len;
				ret.x = one_char_width * range.start;
				ret.width = one_char_width * range.size();
				return ret;
			}

			static std::tuple<cv::Rect, cv::Rect, cv::Rect> getItemUnitResultRects(const std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>& items,
																				   const std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>& units,
																				   const std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>& results) {
				std::vector<std::pair<cv::Rect, cv::Rect>> temp_list;
				for (auto& item : items) {
					cv::Rect item_rect = field_to_rect(item);
					for (auto& result : results) {
						cv::Rect result_rect = field_to_rect(result);
						if (isHorizontalLine(item_rect, result_rect, 0.8f)) {
							temp_list.push_back(std::make_pair(item_rect, result_rect));
						}
					}
				}

				std::vector<std::tuple<cv::Rect, cv::Rect, cv::Rect>> ret;
				for (auto& rect_pair : temp_list) {
					bool flag = false;
					for (auto& unit : units) {
						cv::Rect unit_rect = field_to_rect(unit);
						if (isHorizontalLine(unit_rect, rect_pair.first, 0.8f) ||
							isHorizontalLine(unit_rect, rect_pair.second, 0.8f)) {
							flag = true;
							ret.push_back(std::make_tuple(rect_pair.first, unit_rect, rect_pair.second));
							break;
						}
					}
					if (!flag) {
						ret.push_back(std::make_tuple(rect_pair.first, cv::Rect(0, 0, 0, 0), rect_pair.second));
					}

				}

				if (ret.empty()) {
					return std::make_tuple(cv::Rect(), cv::Rect(), cv::Rect());
				}

				return ret[0];
			}

			static std::pair<cv::Rect, std::wstring> get_vertical_line(const std::vector<std::pair<cv::Rect, std::wstring>>& extracted, cv::Rect rect, float hitting_threshold = 0.8) {
				for (auto& ex_i : extracted) {
					if (isVerticalLine(rect, std::get<0>(ex_i), hitting_threshold)) {
						return ex_i;
					}
				}
				return std::make_pair(cv::Rect(), L"");
			}



			static std::pair<cv::Rect, std::wstring> get_horizontal_line(const std::vector<std::pair<cv::Rect, std::wstring>>& extracted, cv::Rect rect, float hitting_threshold = 0.8) {
				for (auto& ex_i : extracted) {
					if (isHorizontalLine(rect, std::get<0>(ex_i), hitting_threshold)) {
						return ex_i;
					}
				}
				return std::make_pair(cv::Rect(), L"");
			}

			static std::vector<std::pair<cv::Rect, std::wstring>> get_horizontal_lines(const std::vector<std::pair<cv::Rect, std::wstring>>& extracted, cv::Rect rect, float hitting_threshold = 0.8) {
				std::vector<std::pair<cv::Rect, std::wstring>> ret;
				for (auto& ex_i : extracted) {
					if (isHorizontalLine(rect, std::get<0>(ex_i), hitting_threshold)) {
						ret.push_back(ex_i);
					}
				}
				return ret;
			}

			static std::pair<cv::Rect, std::wstring> get_horizontal_join_line(const std::vector<std::pair<cv::Rect, std::wstring>>& extracted, cv::Rect rect, float hitting_threshold = 0.8) {
				const auto& lines = get_horizontal_lines(extracted, rect, hitting_threshold);
				return get_join_line(lines);
			}

			static bool is_inside_item(cv::Rect& rect, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>& items, int index) {
				for (int i = 0; i < items.size(); ++i) {
					if (i == index) {
						continue;
					}
					cv::Rect item_rect = std::get<0>(items[i]);
					if (rect.contains(item_rect.br() - cv::Point(1,1)) && rect.contains(item_rect.tl() + cv::Point(1,1))) {
						return true;
					}
				}

				return false;
			}

			static  std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>> remove_outside(const std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>& src_items) {
				std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>> items = src_items;
				std::sort(items.begin(), items.end(), [](std::tuple<cv::Rect, std::wstring, cv::Range>& a, std::tuple<cv::Rect, std::wstring, cv::Range>& b) {
					return std::get<0>(a).area() < std::get<0>(b).area();
				});
				for (int i = items.size() - 1; i >= 0; --i) {
					auto& item_rect = std::get<0>(items[i]);
					if (is_inside_item(item_rect, items, i)) {
						items.erase(items.begin() + i);
					}
				}
				return items;
			}

			static void remove_not_horizontal(std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>& a,
				std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>& b) {
				for (int i = a.size() - 1; i >= 0; --i) {
					cv::Rect item_rect = std::get<0>(a[i]);
					bool flag = false;
					for (int j = b.size() - 1; j >= 0; --j) {
						cv::Rect result_rect = std::get<0>(b[j]);
						if (isHorizontalLine(item_rect, result_rect, 0.8f)) {
							flag = true;
							break;
						}
					}
					if (!flag) {
						a.erase(a.begin() + i);
					}
				}
			}

			static void remove_not_table(std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>& items,
										 std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>& units,
										 std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>& results) {
				remove_not_horizontal(items, results);
				remove_not_horizontal(results, items);
				remove_not_horizontal(units, items);
			}

			static word get_character_list(const line& line) {
				std::vector<character> ret;

				if (line.size() <= 0) {
					return ret;
				}

				for (int i = 0; i < line.size(); ++i) {
					auto& word = line[i];
					for (auto& character : word) {
						if (ret.size() > 0) {
							ret.back().first.width = character.first.x - ret.back().first.x;
						}
						ret.push_back(character);
					}
					const auto& pre_rect = ret.back().first;
					cv::Rect space_rect = cv::Rect(pre_rect.x + pre_rect.width, pre_rect.y, pre_rect.width / 2, pre_rect.height);
					if (i != line.size() - 1) {
						ret.push_back(std::make_pair(space_rect, L' '));
					}
				}
				return ret;
			}

			static std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>> line_field_to_word_field(const std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>& field,
																								const std::vector<block>& blocks) {
				std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>> ret;
				for (const auto& line_field : field) {
					for (const auto& block : blocks) {
						for (const auto& line : block) {
							if (to_rect(line) == std::get<0>(line_field)) {
								int char_size = 0;
								for (const auto& word : line) {
									char_size += word.size();
								}
								char_size += (line.size() - 1);
								std::wstring line_str = std::get<1>(line_field);
								int line_length = line_str.length();
								float ratio = char_size / (float)line_length;
								cv::Range range = std::get<2>(line_field);
								int field_start = range.start * ratio;
								int field_end = range.end * ratio;
								if (field_end >= char_size) {
									field_end = char_size - 1;
								}

								std::vector<bool> temp_counts(line.size(), false);

								int start_index = 0;
								for (int i = 0; i < line.size(); ++i) {
									const auto& word = line[i];
									int end_index = start_index + word.size();
									int gap = std::min(field_end, end_index) - std::max(field_start, start_index);
									if (gap > word.size() * 0.8 && word.size() > 1) {
										temp_counts[i] = true;
									}
									start_index = end_index + 1;
								}
								cv::Rect new_rect = cv::Rect();
								for (int i = 0; i < line.size(); ++i) {
									if (temp_counts[i]) {
										new_rect = new_rect | to_rect(line[i]);
									}
								}

								if (new_rect.width == 0) {
									auto character_list = get_character_list(line);
									new_rect = character_list[field_start].first | character_list[field_end].first;
									//int start_index = 0;
									//std::vector<float> temp_ratios(line.size(), 0.0f);
									//for (int i = 0; i < line.size(); ++i) {
									//	const auto& word = line[i];
									//	int end_index = start_index + word.size();
									//	int gap = std::min(field_end, end_index) - std::max(field_start, start_index);
									//	temp_ratios[i] = gap / (float) word.size();
									//	start_index = end_index + 1;
									//}
									//int index = distance(temp_ratios.begin(), std::max_element(temp_ratios.begin(), temp_ratios.end()));
									//new_rect = to_rect(line[index]);
								}

								std::wstring new_str = line_str.substr(range.start, range.end - range.start);
								ret.push_back(std::make_tuple(new_rect, new_str, cv::Range(0, new_str.length())));
							}
						}
					}
				}
				return ret;
			}

			static std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>> pairs_to_fields(const std::vector<std::pair<cv::Rect, std::wstring>>& pairs) {
				std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>> ret;
				for (const auto& pair : pairs) {
					ret.push_back(std::make_tuple(pair.first, pair.second, cv::Range(0, pair.second.length())));
				}

				return ret;
			}

			static std::unordered_map<std::wstring, std::vector<std::tuple<std::wstring, std::wstring, std::wstring>>>
				extract_items_unit_results_using_log_no_pair(const configuration& configuration, const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
				const std::vector<block>& blocks, const cv::Size& image_size, const std::vector<std::pair<cv::Rect, std::wstring>> lot_no_pair) {
				std::unordered_map<std::wstring, std::vector<std::tuple<std::wstring, std::wstring, std::wstring>>> ret;
				auto inner_lot = lot_no_pair;
				if (inner_lot.empty()) {
					inner_lot.push_back(std::make_pair(cv::Rect(), L"EMPTY"));
				}
				for (const auto& lot : inner_lot) {
					ret.emplace(std::make_pair(std::get<1>(lot), std::vector<std::tuple<std::wstring, std::wstring, std::wstring>>()));
				}

				auto item_fields = fields.find(L"ITEMS");
				auto unit_fields = fields.find(L"UNIT");
				auto result_fields = fields.find(L"RESULT");

				if (item_fields == fields.end() || result_fields == fields.end())
					return ret;

				auto& items = remove_outside(fields.at(L"ITEMS"));
				auto& results = remove_outside(fields.at(L"RESULT"));
				std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>> units;
				if (unit_fields != fields.end()) {
					units = remove_outside(fields.at(L"UNIT"));
				}

				if (category == L"NOF") {
					results = pairs_to_fields(lot_no_pair);
				}

				if (category == L"CHIMEI") {
					items = line_field_to_word_field(items, blocks);
					results = line_field_to_word_field(results, blocks);
					units = line_field_to_word_field(units, blocks);
				}

				float hitting_threshold;
				float expanded_ratio;
				float cutting_ratio;
				bool multi_line;
				bool merge;
				bool expand_both_sides;
				bool restrict_right = false;
				float next_line_ratio;
				float restrict_ratio = 0;
				float expanded_ratio_result = 0.0;

				if (category == L"CHIMEI") {
					hitting_threshold = 0.2;
					expanded_ratio = 0.0;
					cutting_ratio = 40;
					multi_line = true;
					merge = false;
					expand_both_sides = false;
					next_line_ratio = 4.5;
				}
				else {
					hitting_threshold = 0.2;
					expanded_ratio = 0.0;
					cutting_ratio = 40;
					multi_line = true;
					merge = false;
					expand_both_sides = false;
					next_line_ratio = 4.5;
				}

				std::vector<std::pair<cv::Rect, std::wstring>> item_values;
				std::vector<std::pair<cv::Rect, std::wstring>> unit_values;
				std::vector<std::pair<cv::Rect, std::wstring>> result_values;


				if (category == L"NOF") {
					result_values = extract_field_values(results, blocks,
						std::bind(find_down_characters, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 10, false, false, 0),
						default_preprocess,
						default_extract,
						std::bind(&coa_document_recognizer::postprocess_item_unit, std::placeholders::_1, category));

					items = pairs_to_fields(result_values);

					item_values = extract_field_values(items, blocks,
						std::bind(find_far_left_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						default_preprocess,
						default_extract,
						std::bind(&coa_document_recognizer::postprocess_item_name, std::placeholders::_1, category));


					for (int i = item_values.size() - 1; i >= 0; --i) {
						bool remove_flag = false;
						for (int j = 0; j < item_values.size(); ++j) {
							if (i == j) {
								continue;
							}
							if (item_values[i].first == item_values[j].first) {
								remove_flag = true;
								break;
							}
						}
						if (remove_flag) {
							item_values.erase(item_values.begin() + i);
						}
					}

					item_values.erase(std::unique(item_values.begin(), item_values.end()), item_values.end());
				}
				else if (category == L"CHIMEI") {
					item_values = extract_field_values(items, blocks,
						std::bind(find_coa_nearest_down_lines, std::placeholders::_1, std::placeholders::_2, hitting_threshold, expanded_ratio, cutting_ratio, multi_line, merge, expand_both_sides, next_line_ratio),
						default_preprocess,
						default_extract,
						std::bind(&coa_document_recognizer::postprocess_item_name, std::placeholders::_1, category));


					boost::remove_erase_if(item_values, [](const std::pair<cv::Rect, std::wstring>& item) {
						return std::get<1>(item).size() <= 5; });

					std::sort(std::begin(item_values), std::end(item_values),
						[](const std::pair<cv::Rect, std::wstring>& a, const std::pair<cv::Rect, std::wstring>& b) {
						return to_rect(a).y < to_rect(b).y;
					});

					item_values.erase(std::unique(item_values.begin(), item_values.end()), item_values.end());


					unit_values = extract_field_values(units, blocks,
						std::bind(find_down_characters, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false, false, 0),
						default_preprocess,
						default_extract,
						std::bind(&coa_document_recognizer::postprocess_item_unit, std::placeholders::_1, category));


					std::sort(std::begin(unit_values), std::end(unit_values),
						[](const std::pair<cv::Rect, std::wstring>& a, const std::pair<cv::Rect, std::wstring>& b) {
						return to_rect(a).y < to_rect(b).y;
					});

					unit_values.erase(std::unique(unit_values.begin(), unit_values.end()), unit_values.end());


					result_values = extract_field_values(results, blocks,
						std::bind(find_down_characters, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false, false, 0),
						default_preprocess,
						default_extract,
						std::bind(&coa_document_recognizer::postprocess_item_unit, std::placeholders::_1, category));


					std::sort(std::begin(result_values), std::end(result_values),
						[](const std::pair<cv::Rect, std::wstring>& a, const std::pair<cv::Rect, std::wstring>& b) {
						return to_rect(a).y < to_rect(b).y;
					});

					result_values.erase(std::unique(result_values.begin(), result_values.end()), result_values.end());
				}

				std::vector<std::wstring> ret_items;
				std::vector<std::wstring> ret_unit;
				std::vector<std::wstring> ret_results;

				for (auto& item : item_values) {
					cv::Rect item_rect = std::get<0>(item);
					auto unit = get_horizontal_line(unit_values, item_rect, 0.7);
					auto results = get_horizontal_lines(result_values, item_rect, 0.7);
					if (unit.first.width == 0 && results.empty()) {
						continue;
					}
					else if ((results.size() == 1 && std::get<1>(item).compare(results[0].second) == 0) ||
						std::get<1>(item).compare(unit.second) == 0) {
						continue;
					}
					ret_items.push_back(std::get<1>(item));
					ret_unit.push_back(unit.second);
					for (auto& result : results) {
						ret_results.push_back(result.second);
					}

					for (const auto& lot : lot_no_pair) {
						auto result = get_vertical_line(results, std::get<0>(lot), 0.7);
						ret[std::get<1>(lot)].push_back(std::make_tuple(std::get<1>(item), unit.second, result.second));
					}
				}


				if (category == L"CHIMEI") {
					int size = ret_items.size();
					for (int i = 0; i < size; ++i) {
						if (ret_items[i] == L"Volatile Matter (wt%)") {
							ret_unit[i] = L"%";
						}
						else if (ret_items[i] == L"Mooney(MU)") {
							ret_unit[i] = L"MU";
						}
						else if (ret_items[i] == L"Color (APHA)") {
							ret_unit[i] = L"APHA";
						}
						else if (ret_items[i] == L"Color (APHA)") {
							ret_unit[i] = L"APHA";
						}
						else if (ret_items[i] == L"Gel Content") {
							ret_unit[i] = L"ppm";
						}
						else if (ret_items[i] == L"5% Solution viscosity(cps)") {
							ret_unit[i] = L"cst";
						}
					}
				}

				{
					int size = ret_items.size();
					for (int i = 0; i < size; ++i) {
						for (const auto& lot : inner_lot) {
							auto& ret_list = ret[lot.second];
							std::get<0>(ret_list[i]) = ret_items[i];
							std::get<1>(ret_list[i]) = ret_unit[i];
						}
					}
				}

				//	for (auto& item : item_values) {
				//		cv::Rect item_rect = std::get<0>(item);
				//		auto unit = get_horizontal_line(unit_values, item_rect);
				//		auto result = get_horizontal_line(result_values, item_rect);
				//		if (unit.first.width == 0 && result.first.width == 0) {
				//			continue;
				//		}
				//		else if (std::get<1>(item).compare(result.second) == 0 ||
				//			std::get<1>(item).compare(unit.second) == 0) {
				//			continue;
				//		}

				//		ret_items.push_back(std::get<1>(item));
				//		ret_unit.push_back(unit.second);
				//		ret_results.push_back(result.second);
				//	}


				//	results.emplace_back(std::make_pair(std::get<1>(lot_no_pair[i]), std::make_tuple(ret_items, ret_unit, ret_results)));
				//}

				return ret;
			}

			static std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>> remake_field(std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>& field,
																						   const std::vector<std::pair<cv::Rect, std::wstring>>& lot_numbers,
																						   const std::vector<std::pair<cv::Rect, std::wstring>>& dates) {
				std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>> ret;
				std::vector<bool> lot_flags(lot_numbers.size(), false);
				std::vector<bool> date_flags(dates.size(), false);
				for (const auto& key : field) {
					cv::Rect basic = std::get<0>(key);
					basic.y = 0;
					basic.height = std::numeric_limits<int>::max();

					for (int i = 0; i < lot_numbers.size(); ++i) {
						cv::Rect lot_rect = lot_numbers[i].first;
						cv::Rect temp = basic & lot_rect;
						if (temp.width > std::min(basic.width, lot_rect.width) * 0.7) {
							lot_flags[i] = true;
						}
					}

					for (int i = 0; i < dates.size(); ++i) {
						cv::Rect date_rect = dates[i].first;
						cv::Rect temp = basic & date_rect;
						if (temp.width > std::min(basic.width, date_rect.width) * 0.7) {
							date_flags[i] = true;
						}
					}
				}

				for (int i = 0; i < dates.size(); ++i) {
					if (date_flags[i]) {
						continue;
					}

					cv::Rect basic = dates[i].first;
					basic.y = 0;
					basic.height = std::numeric_limits<int>::max();

					for (int j = 0; j < lot_numbers.size(); ++j) {
						cv::Rect lot_rect = lot_numbers[j].first;
						cv::Rect temp = basic & lot_rect;
						if (temp.width > std::min(basic.width, lot_rect.width) * 0.7) {
							lot_flags[j] = true;
						}
					}
				}

				ret.insert(ret.end(), field.begin(), field.end());

				for (int i = 0; i < dates.size(); ++i) {
					if (date_flags[i]) {
						continue;
					}
					ret.push_back(std::make_tuple(dates[i].first, dates[i].second, cv::Range(0, dates[i].second.length())));
				}

				for (int i = 0; i < lot_numbers.size(); ++i) {
					if (lot_flags[i]) {
						continue;
					}
					ret.push_back(std::make_tuple(lot_numbers[i].first, lot_numbers[i].second, cv::Range(0, lot_numbers[i].second.length())));
				}

				return ret;
			}

			static unsigned int get_line_type(const std::pair<cv::Rect, std::wstring>& item, const std::vector<std::pair<cv::Rect, std::wstring>>& results) {
				bool result_flag = false;
				bool str_flag = false;

				for (const auto& result : results) {
					if (isHorizontalLine(item.first, result.first, 0.7)) {
						result_flag = true;
						break;
					}
				}
				if (boost::regex_search(item.second, boost::wregex(L"(nm$|[0-9]{3}.{2,3}$)", boost::regex::icase))) {
					str_flag = true;
				}

				if (boost::regex_search(item.second, boost::wregex(L"^maximum[ ]{0,1}recom", boost::regex::icase))) {
					return 16;
				}

				int ret = result_flag ? 2 : 0;
				ret = ret | (str_flag ? 4 : 0);

				return ret;
			}

			static int get_line_type(std::wstring& str) {
				bool start = false;
				bool end = false;
				if (boost::regex_search(str, boost::wregex(L"(appe|vola|bul|parti|visco|ther|densi|ulk|size|mesh|ticle|rmal)", boost::regex::icase))) {
					start = true;
				}

				if (boost::regex_search(str, boost::wregex(L"([)]$|[％%]$|ml$|lity$)", boost::regex::icase))) {
					end = true;
				}

				if (boost::regex_search(str, boost::wregex(L"(.{1,2}ecked|c.{1,2}cked|ch.{1,2}ked|che.{1,2}ed|chec.{1,2}d|check.{1,2})", boost::regex::icase))) {
					return 4;
				}

				return (start ? 1 : 0) | (end ? 2 : 0);
			}

			static std::pair<cv::Rect, std::wstring> get_join_line(const std::vector<std::pair<cv::Rect, std::wstring>>& weihai_items) {
				cv::Rect ret_rect(0, 0, 0, 0);

				std::wstring ret_str;
				for (int i = 0; i < weihai_items.size(); ++i) {
					ret_rect = ret_rect | std::get<0>(weihai_items[i]);
					ret_str = ret_str + std::get<1>(weihai_items[i]) + L" ";
				}
				return std::make_pair(ret_rect, boost::trim_copy(ret_str));
			}

			static std::pair<cv::Rect, std::wstring> get_items_line(const std::vector<std::pair<cv::Rect, std::wstring>>& weihai_items, int start, int end) {
				if (end < start) {
					return std::make_pair(cv::Rect(0,0,0,0), L"");
				}
				return get_join_line(std::vector<std::pair<cv::Rect, std::wstring>>(weihai_items.begin() + start, weihai_items.begin() + end + 1));
			}

			static std::vector<std::pair<cv::Rect, std::wstring>> transform_basf(const std::vector<std::pair<cv::Rect, std::wstring>>& basf_items, const std::vector<std::pair<cv::Rect, std::wstring>>& basf_results) {
				std::vector<std::pair<cv::Rect, std::wstring>> ret;

				std::vector<unsigned int> start_end_flag(basf_items.size(), 0);
				for (int i = 0; i < basf_items.size(); ++i) {
					start_end_flag[i] = get_line_type(basf_items[i], basf_results);
				}

				for (int i = 0; i < basf_items.size(); ++i) {
					if ((start_end_flag[i] & 4) != 0 && (start_end_flag[i] & 2) != 0) {
						if (i - 1 >= 0 && (start_end_flag[i - 1] & 2) == 0) {
							start_end_flag[i - 1] = start_end_flag[i - 1] | 8;
						}
						if (i - 2 >= 0 && (start_end_flag[i - 2] & 2) == 0) {
							if ((start_end_flag[i - 2] & 2) != 0) {
								start_end_flag[i - 2] = start_end_flag[i - 2] | 8;
							}
						}
					}
				}

				for (int i = 0; i < basf_items.size(); ++i) {
					if ((start_end_flag[i] & 16) != 0) {
						break;
					} else if ((start_end_flag[i] & 4) != 0) {
						if (i - 1 >= 0 && (start_end_flag[i - 1] & 8) != 0) {
							ret.push_back(std::make_pair(basf_items[i].first, basf_items[i - 1].second + L" " + basf_items[i].second));
						}
						if (i - 2 >= 0 && (start_end_flag[i - 2] & 8) != 0) {
							ret.push_back(std::make_pair(basf_items[i].first, basf_items[i - 2].second + L" " + basf_items[i].second));
						}
					} else if ((start_end_flag[i] & 2) != 0) {
						if (i + 1 < basf_items.size() &&
							(start_end_flag[i + 1] & 8) == 0 &&
							(start_end_flag[i + 1] & 2) == 0 &&
							(start_end_flag[i + 1] & 16) == 0) {
							ret.push_back(get_items_line(basf_items, i, i + 1));
							++i;
							continue;
						}
						ret.push_back(get_items_line(basf_items, i, i));
					}
				}


				boost::remove_erase_if(ret, [](const std::pair<cv::Rect, std::wstring>& item) {
					return std::get<0>(item).width == 0 || std::get<1>(item) == L"";
				});

				return ret;
			}

			static std::vector<std::pair<cv::Rect, std::wstring>> transform_weihai(std::vector<std::pair<cv::Rect, std::wstring>>& weihai_items) {
				std::vector<std::pair<cv::Rect, std::wstring>> ret;

				std::vector<int> start_end_flag(weihai_items.size(), 0);
				for (int i = 0; i < weihai_items.size(); ++i) {
					std::wstring& str = weihai_items[i].second;
					start_end_flag[i] = get_line_type(str);
				}

				int save_insert_start = -1;
				int save_insert_end = -1;
				int start_index = -1;
				int end_index = -1;
				bool finish = false;
				for (int i = 0; i < weihai_items.size(); ++i) {
					if ((start_end_flag[i] & 1) != 0 && start_index != -1 && end_index != -1) {
						if (save_insert_end != start_index - 1) {
							ret.push_back(get_items_line(weihai_items, save_insert_end + 1, start_index - 1));
						}
						ret.push_back(get_items_line(weihai_items, start_index, end_index));
						save_insert_start = start_index;
						save_insert_end = end_index;
						start_index = -1;
						end_index = -1;
					}
					if ((start_end_flag[i] & 1) != 0 && start_index == -1) {
						start_index = i;
					}
					if ((start_end_flag[i] & 2) != 0) {
						end_index = i;
					}
					if ((start_end_flag[i] & 4) != 0) {
						if (save_insert_end != -1 && save_insert_end < i - 2) {
							ret.push_back(get_items_line(weihai_items, save_insert_end + 1, i - 2));
						}
						finish = true;
						break;
					}
				}
				if (!finish && start_index != -1) {
					if (save_insert_end != -1) {
						ret.push_back(get_items_line(weihai_items, save_insert_end + 1, weihai_items.size() - 1));
					}
					else {
						ret.push_back(get_items_line(weihai_items, start_index, weihai_items.size() - 1));
					}
				}

				boost::remove_erase_if(ret, [](const std::pair<cv::Rect, std::wstring>& item) {
					return std::get<0>(item).width == 0 || std::get<1>(item) == L"";
				});

				return ret;
			}

			static void transform_list(const std::unordered_map<std::wstring, std::vector<std::tuple<std::wstring, std::wstring, std::wstring>>>& lot_item_map,
				std::vector<std::wstring>& items, std::vector<std::wstring>& units, std::vector<std::wstring>& results) {
				std::vector<std::wstring> lot_numbers;
				std::transform(lot_item_map.begin(), lot_item_map.end(), std::back_inserter(lot_numbers), [](const std::pair<std::wstring, std::vector<std::tuple<std::wstring, std::wstring, std::wstring>>>& result) {
					return result.first;
				});
				for (const auto& lot : lot_numbers) {
					for (const auto& tuple : lot_item_map.at(lot)) {
						items.push_back(std::get<0>(tuple));
						units.push_back(std::get<1>(tuple));
						results.push_back(std::get<2>(tuple));
					}
				}
			}

			static std::unordered_map<std::wstring, std::vector<std::tuple<std::wstring, std::wstring, std::wstring>>>
				extract_items_unit_results(const configuration& configuration, const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
				const std::vector<block>& blocks, const cv::Size& image_size, const std::vector<std::pair<cv::Rect, std::wstring>>& lot_numbers, const std::vector<std::pair<cv::Rect, std::wstring>>& dates)
			{
				std::unordered_map<std::wstring, std::vector<std::tuple<std::wstring, std::wstring, std::wstring>>> ret;
				auto inner_lot = lot_numbers;
				if (inner_lot.empty()) {
					inner_lot.push_back(std::make_pair(cv::Rect(), L"EMPTY"));
				}
				for (const auto& lot : inner_lot) {
					ret.emplace(std::make_pair(std::get<1>(lot), std::vector<std::tuple<std::wstring, std::wstring, std::wstring>>()));
				}

				auto item_fields = fields.find(L"ITEMS");
				auto unit_fields = fields.find(L"UNIT");
				auto result_fields = fields.find(L"RESULT");
				if (item_fields == fields.end() || result_fields == fields.end())
					return ret;

				auto& items = remove_outside(fields.at(L"ITEMS"));
				auto& results = remove_outside(fields.at(L"RESULT"));
				std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>> units;
				if (unit_fields != fields.end()) {
					units = remove_outside(fields.at(L"UNIT"));
				}
				if (items.size() != 0 && results.size() != 0) {
					remove_not_table(items, units, results);
					if (items.size() == 0 || results.size() == 0) {
						items = remove_outside(fields.at(L"ITEMS"));
						results = remove_outside(fields.at(L"RESULT"));
						if (unit_fields != fields.end()) {
							units = remove_outside(fields.at(L"UNIT"));
						}
					}
				}
				if (category == L"LANXESS" || category == L"LG HNH" || category == L"BASF" || category == L"XIAMETER" || category == L"ALBEMARLE") {
					items = line_field_to_word_field(items, blocks);
					results = line_field_to_word_field(results, blocks);
					units = line_field_to_word_field(units, blocks);
				}
				if (category == L"DONGSUNG") {
					results = remake_field(results, lot_numbers, dates);
				}


				float hitting_threshold;
				float expanded_ratio;
				float cutting_ratio;
				bool multi_line;
				bool merge;
				bool expand_both_sides;
				bool restrict_right = false;
				float next_line_ratio;
				float restrict_ratio = 0;
				float expanded_ratio_result = 0.0;
				if (category == L"POWERTECH") {
					hitting_threshold = 0.2;
					expanded_ratio = 4.0;
					cutting_ratio = 40;
					multi_line = true;
					merge = false;
					expand_both_sides = true;
					next_line_ratio = 4.5;
				}
				else if (category == L"LG HNH") {
					hitting_threshold = 0.2;
					expanded_ratio = 3.0;
					cutting_ratio = 40;
					multi_line = true;
					merge = false;
					expand_both_sides = true;
					next_line_ratio = 10;
				}
				else if (category == L"BASF") {
					hitting_threshold = 0.2;
					expanded_ratio = 4.0;
					cutting_ratio = 40;
					multi_line = true;
					merge = false;
					expand_both_sides = true;
					next_line_ratio = 3;
					restrict_ratio = 0;
					restrict_right = true;
				}
				else if (category == L"SUNGU") {
					hitting_threshold = 0.2;
					expanded_ratio = 0.0;
					cutting_ratio = 40;
					multi_line = true;
					merge = false;
					expand_both_sides = false;
					next_line_ratio = 12;
				}
				else if (category == L"BASF_B") {
					hitting_threshold = 0.2;
					expanded_ratio = 0.3;
					cutting_ratio = 40;
					multi_line = true;
					merge = false;
					expand_both_sides = true;
					next_line_ratio = 4.5;
				}
				else if (category == L"XIAMETER") {
					hitting_threshold = 0.2;
					expanded_ratio = 6.0;
					cutting_ratio = 40;
					multi_line = true;
					merge = false;
					expand_both_sides = true;
					next_line_ratio = 2;
					restrict_ratio = 2.5;
					restrict_right = true;
					expanded_ratio_result = 2.5;
				}
				else if (category == L"DUKSAN") {
					hitting_threshold = 0.2;
					expanded_ratio = 5.0;
					cutting_ratio = 40;
					multi_line = true;
					merge = false;
					expand_both_sides = true;
					next_line_ratio = 4.5;
				}
				else if (category == L"ALBEMARLE") {
					hitting_threshold = 0.2;
					expanded_ratio = 0.0;
					cutting_ratio = 40;
					multi_line = true;
					merge = false;
					expand_both_sides = false;
					next_line_ratio = 1.5;
				}
				else {
					hitting_threshold = 0.2;
					expanded_ratio = 0.0;
					cutting_ratio = 40;
					multi_line = true;
					merge = false;
					expand_both_sides = false;
					next_line_ratio = 4.5;
				}

				std::vector<std::pair<cv::Rect, std::wstring>> item_values;
				std::vector<std::pair<cv::Rect, std::wstring>> unit_values;
				std::vector<std::pair<cv::Rect, std::wstring>> result_values;

				if (category == L"LANXESS" || category == L"LG HNH" || category == L"BASF" || category == L"XIAMETER") {
					if (category == L"BASF") {
						item_values = extract_field_values(items, blocks,
							std::bind(find_coa_nearest_down_words2, std::placeholders::_1, std::placeholders::_2, hitting_threshold, expanded_ratio,
							cutting_ratio, multi_line, merge, expand_both_sides, next_line_ratio, restrict_right, restrict_ratio),
							default_preprocess,
							default_extract,
							default_postprocess);

					}
					else {
						item_values = extract_field_values(items, blocks,
							std::bind(find_coa_nearest_down_words, std::placeholders::_1, std::placeholders::_2, hitting_threshold, expanded_ratio,
							cutting_ratio, multi_line, merge, expand_both_sides, next_line_ratio, restrict_right, restrict_ratio),
							default_preprocess,
							default_extract,
							default_postprocess);
					}


					unit_values = extract_field_values(units, blocks,
						std::bind(find_coa_nearest_down_words, std::placeholders::_1, std::placeholders::_2, 0.2, 0.0, 40, true, false, true, 100, false, 0),
						default_preprocess,
						default_extract,
						default_postprocess);

					result_values = extract_field_values(results, blocks,
						std::bind(find_coa_nearest_down_words, std::placeholders::_1, std::placeholders::_2, 0.2, expanded_ratio_result, 40, true, false, true, 100, false, 0),
						default_preprocess,
						default_extract,
						default_postprocess);

					if (category == L"BASF") {
						item_values = transform_basf(item_values, result_values);
					}
				}
				else {
					if (category == L"DUKSAN") {
						boost::remove_erase_if(items, [](const std::tuple<cv::Rect, std::wstring, cv::Range>& item) {
							return std::get<1>(item).size() > 6;
						});
					}

					item_values = extract_field_values(items, blocks,
						std::bind(find_coa_nearest_down_lines, std::placeholders::_1, std::placeholders::_2, hitting_threshold, expanded_ratio, cutting_ratio, multi_line, merge, expand_both_sides, next_line_ratio),
						default_preprocess,
						default_extract,
						std::bind(&coa_document_recognizer::postprocess_item_name, std::placeholders::_1, category));

					if (category == L"POWERTECH" && item_values.size() <= 0 && fields.find(L"APPEARANCE") != fields.end()) {
						item_values.emplace_back(std::make_pair(std::get<0>(fields.at(L"APPEARANCE")[0]), L"Appearance"));
						auto sugested_item_values = extract_field_values(fields.at(L"APPEARANCE"), blocks,
							std::bind(find_coa_nearest_down_lines, std::placeholders::_1, std::placeholders::_2, 0.2, 0.0, 40, true, false, false, 4.5),
							default_preprocess,
							default_extract,
							std::bind(&coa_document_recognizer::postprocess_item_name, std::placeholders::_1, category));
						for (auto item : sugested_item_values) {
							item_values.emplace_back(item);
						}
					}
					else if (category == L"CHIMEI") {
						boost::remove_erase_if(item_values, [](const std::pair<cv::Rect, std::wstring>& item) {
							return std::get<1>(item).size() <= 5; });

						std::sort(std::begin(item_values), std::end(item_values),
							[](const std::pair<cv::Rect, std::wstring>& a, const std::pair<cv::Rect, std::wstring>& b) {
							return to_rect(a).y < to_rect(b).y;
						});

						item_values.erase(std::unique(item_values.begin(), item_values.end()), item_values.end());
					}

					unit_values = extract_field_values(units, blocks,
						std::bind(find_down_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						default_preprocess,
						default_extract,
						std::bind(&coa_document_recognizer::postprocess_item_unit, std::placeholders::_1, category));

					if (category == L"CHIMEI") {
						std::sort(std::begin(unit_values), std::end(unit_values),
							[](const std::pair<cv::Rect, std::wstring>& a, const std::pair<cv::Rect, std::wstring>& b) {
							return to_rect(a).y < to_rect(b).y;
						});

						unit_values.erase(std::unique(unit_values.begin(), unit_values.end()), unit_values.end());
					}

					result_values = extract_field_values(results, blocks,
						std::bind(find_down_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						default_preprocess,
						default_extract,
						std::bind(&coa_document_recognizer::postprocess_item_result, std::placeholders::_1, category));

					if (category == L"DUKSAN" && result_values.empty() && fields.find(L"LOT NO") != fields.end()) {
						auto lot_field = line_field_to_word_field(fields.at(L"LOT NO"), blocks);
						result_values = extract_field_values(lot_field, blocks,
							std::bind(find_down_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
							default_preprocess,
							default_extract,
							std::bind(&coa_document_recognizer::postprocess_item_result, std::placeholders::_1, category));
					}
				}

				std::vector<std::wstring> ret_items;
				std::vector<std::wstring> ret_unit;
				std::vector<std::wstring> ret_results;

				if (category == L"DONGSUNG") {
					for (auto& item : item_values) {
						cv::Rect item_rect = std::get<0>(item);
						auto unit = get_horizontal_line(unit_values, item_rect);
						auto results = get_horizontal_lines(result_values, item_rect);
						auto lot_rect = get_horizontal_line(inner_lot, item_rect, 0.7);
						auto date_rect = get_horizontal_line(dates, item_rect, 0.7);
						if (lot_rect.first.width != 0 || date_rect.first.width != 0) {
							continue;
						}
						else if (unit.first.width == 0 && results.empty()) {
							continue;
						}
						else if ((results.size() == 1 && std::get<1>(item).compare(results[0].second) == 0) ||
							std::get<1>(item).compare(unit.second) == 0) {
							continue;
						}
						ret_items.push_back(std::get<1>(item));
						ret_unit.push_back(unit.second);
						for (auto& result : results) {
							ret_results.push_back(result.second);
						}

						for (const auto& lot : inner_lot) {
							auto result = get_vertical_line(results, std::get<0>(lot), 0.7);
							ret[std::get<1>(lot)].push_back(std::make_tuple(std::get<1>(item), unit.second, result.second));
						}
					}
				}
				else if (category == L"UNID") {
					for (auto& item : item_values) {
						cv::Rect item_rect = std::get<0>(item);
						auto unit = get_horizontal_line(unit_values, item_rect, 0.7);
						auto result = get_horizontal_line(result_values, item_rect, 0.7);
						if (unit.first.width == 0 && result.first.width == 0) {
							continue;
						}
						else if ((results.size() == 1 && std::get<1>(item).compare(result.second) == 0) ||
							std::get<1>(item).compare(unit.second) == 0) {
							continue;
						}
						ret_items.push_back(std::get<1>(item));
						ret_unit.push_back(unit.second);
						ret_results.push_back(result.second);

						for (const auto& lot : inner_lot) {
							ret[std::get<1>(lot)].push_back(std::make_tuple(std::get<1>(item), unit.second, result.second));
						}
					}
				}
				else if (category == L"BASF_B") {
					int min_x = std::numeric_limits<int>::max();
					for (auto& item : item_values) {
						cv::Rect item_rect = std::get<0>(item);
						if (item_rect.x < min_x)
							min_x = item_rect.x;
					}

					for (int i = item_values.size() - 1; i >= 0; i--) {
						auto item = item_values[i];
						cv::Rect item_rect = std::get<0>(item);
						if (item_rect.x > min_x + (image_size.width * 0.15)) {
							unit_values.emplace_back(item);
							item_values.erase(item_values.begin() + i);
						}
					}

					for (auto& item : item_values) {
						cv::Rect item_rect = std::get<0>(item);
						auto unit = get_horizontal_line(unit_values, item_rect);
						auto result = get_horizontal_line(result_values, item_rect);
						if (unit.first.width == 0 && result.first.width == 0) {
							continue;
						}
						else if (std::get<1>(item).compare(result.second) == 0 ||
							std::get<1>(item).compare(unit.second) == 0) {
							continue;
						}
						ret_items.push_back(std::get<1>(item));
						ret_unit.push_back(unit.second);
						ret_results.push_back(result.second);

						for (const auto& lot : inner_lot) {
							ret[std::get<1>(lot)].push_back(std::make_tuple(std::get<1>(item), unit.second, result.second));
						}
					}
				}
				else if (category == L"DUKSAN") {
					for (auto& item : item_values) {
						cv::Rect item_rect = std::get<0>(item);
						auto unit = get_horizontal_line(unit_values, item_rect, 0.4);
						auto result = get_horizontal_line(result_values, item_rect, 0.4);
						if (unit.first.width == 0 && result.first.width == 0) {
							continue;
						}
						else if (std::get<1>(item).compare(result.second) == 0 ||
							std::get<1>(item).compare(unit.second) == 0) {
							continue;
						}
						ret_items.push_back(std::get<1>(item));
						ret_unit.push_back(unit.second);
						ret_results.push_back(result.second);

						for (const auto& lot : inner_lot) {
							ret[std::get<1>(lot)].push_back(std::make_tuple(std::get<1>(item), unit.second, result.second));
						}
					}
				}
				else if (category == L"WEIHAI") {
					item_values = transform_weihai(item_values);
					for (auto& item : item_values) {
						cv::Rect item_rect = std::get<0>(item);
						auto unit = get_horizontal_join_line(unit_values, item_rect, 0.7);
						auto result = get_horizontal_join_line(result_values, item_rect, 0.7);
						cv::Rect coll_unit = item_rect & unit.first;
						cv::Rect coll_result = item_rect & result.first;
						if (unit.first.width == 0 && result.first.width == 0) {
							continue;
						}
						else if (std::get<1>(item).compare(result.second) == 0 ||
							std::get<1>(item).compare(unit.second) == 0 ||
							unit.second.compare(result.second) == 0) {
							continue;
						}
						else if (coll_result.width > std::min(item_rect.width, result.first.width) * 0.8 ||
							coll_unit.width > std::min(item_rect.width, unit.first.width) * 0.8) {
							continue;
						}
						ret_items.push_back(std::get<1>(item));
						ret_unit.push_back(unit.second);
						ret_results.push_back(result.second);

						for (const auto& lot : inner_lot) {
							ret[std::get<1>(lot)].push_back(std::make_tuple(std::get<1>(item), unit.second, result.second));
						}
					}
				}
				else if (category == L"ALBEMARLE") {
					for (auto& item : item_values) {
						cv::Rect item_rect = std::get<0>(item);
						auto unit = get_horizontal_line(unit_values, item_rect, 0.7);
						auto result = get_horizontal_line(result_values, item_rect, 0.7);
						if (unit.first.width == 0 && result.first.width == 0) {
							continue;
						}
						else if ((results.size() == 1 && std::get<1>(item).compare(result.second) == 0)
							|| std::get<1>(item).compare(unit.second) == 0) {
							continue;
						}
						ret_items.push_back(std::get<1>(item));
						ret_unit.push_back(unit.second);
						ret_results.push_back(result.second);

						for (const auto& lot : inner_lot) {
							ret[std::get<1>(lot)].push_back(std::make_tuple(std::get<1>(item), unit.second, result.second));
						}
					}
				}
				else {
					for (auto& item : item_values) {
						cv::Rect item_rect = std::get<0>(item);
						auto unit = get_horizontal_line(unit_values, item_rect, 0.7);
						auto result = get_horizontal_line(result_values, item_rect, 0.7);
						cv::Rect coll_unit = item_rect & unit.first;
						cv::Rect coll_result = item_rect & result.first;
						if (unit.first.width == 0 && result.first.width == 0) {
							continue;
						}
						else if (std::get<1>(item).compare(result.second) == 0 ||
								 std::get<1>(item).compare(unit.second) == 0 ||
								 unit.second.compare(result.second) == 0) {
							continue;
						}
						else if (coll_result.width > std::min(item_rect.width, result.first.width) * 0.8 ||
								 coll_unit.width > std::min(item_rect.width, unit.first.width) * 0.8) {
							continue;
						}
						ret_items.push_back(std::get<1>(item));
						ret_unit.push_back(unit.second);
						ret_results.push_back(result.second);

						for (const auto& lot : inner_lot) {
							ret[std::get<1>(lot)].push_back(std::make_tuple(std::get<1>(item), unit.second, result.second));
						}
					}
				}


				if (category == L"LG HNH") {
					int size = ret_items.size();
					for (int i = 0; i < size; ++i) {
						if (ret_unit[i].length() != 0) {
							continue;
						}
						auto& item_text = ret_items[i];
						int first_index = item_text.find(L"(");
						if (std::wstring::npos != first_index && L')' == item_text.back()) {
							ret_unit[i] = item_text.substr(first_index, item_text.length() - first_index);
							ret_items[i] = boost::trim_copy(item_text.substr(0, first_index));
						}
					}
				}

				if (category == L"POWERTECH") {
					int size = ret_items.size();
					for (int i = 0; i < size; ++i) {
						if (ret_items[i] == L"Appearance") {
							ret_unit[i] = L"";
						}
						else if (ret_items[i] == L"Viscosity (at 25°C)") {
							ret_unit[i] = L"cPs";
						}
						else if (ret_items[i] == L"pH") {
							ret_unit[i] = L"";
						}
						else if (ret_items[i] == L"Non-Volatile") {
							ret_unit[i] = L"wt.%";
						}
						else if (ret_items[i] == L"Particle size") {
							ret_unit[i] = L"nm";
						}
					}
				}
				else if (category == L"UNID") {
					int size = ret_items.size();
					for (int i = 0; i < size; ++i) {
						if (ret_items[i] == L"외관") {
							ret_unit[i] = L"";
						}
						else if (ret_items[i] == L"성상") {
							ret_unit[i] = L"EA";
						}
						else if (ret_items[i].find(L"K2CO3") != std::wstring::npos
							|| ret_items[i].find(L"KCI") != std::wstring::npos
							|| ret_items[i].find(L"K2SO4") != std::wstring::npos
							|| ret_items[i].find(L"CA") != std::wstring::npos
							|| ret_items[i].find(L"Mg") != std::wstring::npos) {
							ret_unit[i] = L"%";
						}
						else if (ret_items[i].find(L"Fe") != std::wstring::npos
							|| ret_items[i].find(L"중금속 pb") != std::wstring::npos
							|| ret_items[i].find(L"As") != std::wstring::npos) {
							ret_unit[i] = L"ppm";
						}
						else if (ret_items[i].find(L"비중") != std::wstring::npos) {
							ret_unit[i] = L"g/cm3";
						}
						else if (ret_items[i] == L"Particle size") {
							ret_unit[i] = L"nm";
						}
					}
				}
				else if (category == L"SKGC"){
					int size = ret_items.size();
					std::vector<std::wstring> unit_regex = { L"%$", L"ppm$", L"KO[HUI]{1,2}/g$"};
					std::vector<std::wstring> unit_str = { L"%", L"ppm", L"KOH/g" };
					for (int i = 0; i < size; ++i) {
						int before = ret_items[i].length();
						for (int u_i = 0; u_i < unit_regex.size(); ++u_i) {
							ret_items[i] = boost::regex_replace(ret_items[i], boost::wregex(unit_regex[u_i]), L"");
							int after = ret_items[i].length();
							if (after != before) {
								ret_unit[i] = unit_str[u_i];
								break;
							}
						}
					}
				}
				else if (category == L"WEIHAI") {
					int size = ret_items.size();
					std::vector<std::wstring> unit_regex = { L"[,， ]{0,2}[％%]$", L"[,， ]{0,2}g/ml$"};
					std::vector<std::wstring> unit_str = { L"%", L"g/ml"};
					for (int i = 0; i < size; ++i) {
						int before = ret_items[i].length();
						for (int u_i = 0; u_i < unit_regex.size(); ++u_i) {
							ret_items[i] = boost::regex_replace(ret_items[i], boost::wregex(unit_regex[u_i]), L"");
							int after = ret_items[i].length();
							if (after != before) {
								ret_unit[i] = unit_str[u_i];
								break;
							}
						}
					}
				}
				else if (category == L"BASF_B") {
					int size = ret_items.size();
					for (int i = 0; i < size; ++i) {
						if (ret_items[i] == L"Assay") {
							ret_unit[i] = L"(wt%)";
						}
						else if (ret_items[i] == L"WATER") {
							ret_unit[i] = L"(wt%)";
						}
						else if (ret_items[i] == L"WATER") {
							ret_unit[i] = L"";
						}
					}
				}
				else if (category == L"DUKSAN") {
					int size = ret_items.size();
					for (int i = 0; i < size; ++i) {
						if (ret_items[i] == L"Appearance" || ret_items[i] == L"Identification A" || ret_items[i] == L"Identification B" || ret_items[i] == L"Solubility in Water") {
							ret_unit[i] = L"";
						}
						else if (ret_items[i] == L"Assay" || ret_items[i] == L"Chloride (Cl)" || ret_items[i] == L"Sulfate (SO4)"
							|| ret_items[i] == L"Heavy Metals(as Pb)" || ret_items[i] == L"Iron (Fe)" || ret_items[i] == L"Alkality (as NaOH)" || ret_items[i] == L"Dibasic salts (Na2HPO4)") {
							ret_unit[i] = L"%";
						}
						else if (ret_items[i] == L"Arsenic (As)") {
							ret_unit[i] = L"ppm";
						}
					}
				}
				else if (category == L"ALBEMARLE") {
					int size = ret_items.size();
					for (int i = 0; i < size; ++i) {
						if (ret_items[i] == L"Water" || ret_items[i] == L"Iron" || ret_items[i] == L"Hydrolizable Bromide" || ret_items[i] == L"Ionic Bromide") {
							ret_unit[i] = L"ppm";
						}
						else if (ret_items[i] == L"Bromine Content" || ret_items[i] == L"TBBPA CONTENT") {
							ret_unit[i] = L"Wt%";
						}
						else if (ret_items[i] == L"Initial Melting Point") {
							ret_unit[i] = L"°C";
						}
					}
				}
				else if (category == L"CHIMEI") {
					int size = ret_items.size();
					for (int i = 0; i < size; ++i) {
						if (ret_items[i] == L"Volatile Matter (wt%)") {
							ret_unit[i] = L"%";
						}
						else if (ret_items[i] == L"Mooney(MU)") {
							ret_unit[i] = L"MU";
						}
						else if (ret_items[i] == L"Color (APHA)") {
							ret_unit[i] = L"APHA";
						}
						else if (ret_items[i] == L"Color (APHA)") {
							ret_unit[i] = L"APHA";
						}
						else if (ret_items[i] == L"Gel Content") {
							ret_unit[i] = L"ppm";
						}
						else if (ret_items[i] == L"5% Solution viscosity(cps)") {
							ret_unit[i] = L"cst";
						}
					}
				}
				{
					int size = ret_items.size();
					for (int i = 0; i < size; ++i) {
						for (const auto& lot : inner_lot) {
							auto& ret_list = ret[lot.second];
							std::get<0>(ret_list[i]) = ret_items[i];
							std::get<1>(ret_list[i]) = ret_unit[i];
						}
					}
				}

				//std::vector<std::wstring> temp_items;
				//std::vector<std::wstring> temp_units;
				//std::vector<std::wstring> temp_results;
				//std::vector<std::vector<std::wstring>> temp_list;
				//bool flag = false;
				//for (const auto& lot : inner_lot) {
				//	auto ret_list = ret[lot.second];
				//	std::vector<std::wstring> temp;
				//	for (int i = 0; i < ret_list.size(); ++i) {
				//		if (!flag) {
				//			temp_items.push_back(std::get<0>(ret_list[i]));
				//			temp_units.push_back(std::get<1>(ret_list[i]));
				//		}
				//		temp.push_back(std::get<2>(ret_list[i]));
				//	}
				//	flag = true;
				//	temp_list.push_back(temp);
				//}

				//for (int i = 0; i < temp_list[0].size(); ++i) {
				//	for (auto& check : temp_list) {
				//		temp_results.push_back(check[i]);
				//	}
				//}

				//for (int i = 0; i < temp_results.size(); ++i) {
				//	if (temp_results[i].compare(ret_results[i]) != 0) {
				//		wprintf(L"results temp[%s] ret[%s]\n", temp_results[i].c_str(), ret_results[i].c_str());
				//	}
				//}
				//for (int i = 0; i < temp_items.size(); ++i) {
				//	if (temp_items[i].compare(ret_items[i]) != 0) {
				//		wprintf(L"items temp[%s] ret[%s]\n", temp_items[i].c_str(), ret_items[i].c_str());
				//	}
				//	if (temp_units[i].compare(ret_unit[i]) != 0) {
				//		wprintf(L"units temp[%s] ret[%s]\n", temp_units[i].c_str(), ret_unit[i].c_str());
				//	}
				//}


				return ret;
			}

			static std::wstring
				postprocess_item_name(const std::wstring& item_name, const std::wstring& category)
			{
				if (item_name.length() > 30)
					return L"";

				auto cleaned = item_name;

				if (category == L"POWERTECH") {
					auto temp = boost::regex_replace(cleaned, boost::wregex(L"[^a-z]", boost::regex::icase), L"");
					if (temp.size() < 2) {
						return L"";
					}

					cleaned = boost::regex_replace(item_name, boost::wregex(L"Particlesize"), L"Particle size");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"appearance"), L"Appearance");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"Viscosity ?[(]at ?2(.*)"), L"Viscosity (at 25°C)");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"Non ?[ᅳ-] ?Vola(.*)"), L"Non-Volatile");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"pl(.*)"), L"pH");
				}
				else if (category == L"UNID") {
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"KOH ?[\(\[C]Totalalka(.*)"), L"KOH (Total alkali as KOH)");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"K2CO3 ?[\(\[C]Totalalka(.*)"), L"K2CO3 (Total alkali as K2CO3)");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"(.*)토트(.*)"), L"");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"(.*)구상(.*)"), L"");
				}
				else if (category == L"BASF_B") {
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"WA(.*)ER"), L"WATER");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"COLOR, ?Pt-Co"), L"COLOR, Pt-Co");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"wt", boost::regex::icase), L"wt");
				}
				else if (category == L"DUKSAN") {
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"IdentificationA"), L"Identification A");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"Identification[B8]"), L"Identification B");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"SolubilityinWater"), L"Solubility in Water");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"Chloride[(]Cl[)]"), L"Chloride (Cl)");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"Sulfate[(]SO4[)]"), L"Sulfate (SO4)");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"HeavyMetals[(]asPb[)]"), L"Heavy Metals(as Pb)");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"Iron[(]Fe[)]"), L"Iron (Fe)");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"Arsenic[(]As[)]"), L"Arsenic (As)");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"Alkality[(]asNaOH[)]"), L"Alkality (as NaOH)");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"Dibasicsalts[(]Na2HPO4[)]"), L"Dibasic salts (Na2HPO4)");
				}
				else if (category == L"ALBEMARLE") {
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"wa[a-z]er", std::wregex::icase), L"Water");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"[7il]r[aoe]n", std::wregex::icase), L"Iron");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"ionic:?", std::wregex::icase), L"Ionic");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"Hy[ad]ro[lir1!]{2}[tz]ab[1il]e", std::wregex::icase), L"Hydrolizable");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"Iniciadi\\.|Initial\\.|initial", std::wregex::icase), L"Initial");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"poi\\.?n[ut]", std::wregex::icase), L"Point");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"[Bb3e]r[o0c]", std::wregex::icase), L"Bro");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"(?:nti|mi|rm\\.|nm\\.|roi|nti|m1)de"), L"mide");

					cleaned = boost::regex_replace(cleaned, boost::wregex(L"Con[ct]ent\\.?"), L"Content");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"COETERT"), L"CONTENT");

					cleaned = boost::regex_replace(cleaned, boost::wregex(L"73"), L"TB");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"B3|3B|BS\\?|33"), L"BB");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"PZt"), L"PA");

					cleaned = boost::regex_replace(cleaned, boost::wregex(L"InitialMelting"), L"Initial Melting");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"TBBPACONTENT"), L"TBBPA CONTENT");
				}
				else if (category == L"CHIMEI") {
					cleaned = boost::regex_replace(boost::to_upper_copy(cleaned), boost::wregex(L" "), L"");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"VVI%"), L"WT%");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"A[A-Z]HA"), L"APHA");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"57C"), L"5%");

					cleaned = boost::regex_replace(cleaned, boost::wregex(L"VOLATILEMATTER[(]WT%[)]"), L"Volatile Matter (wt%)");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"MOONEY[(]MU[)]"), L"Mooney(MU)");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"COLOR[(]APHA[)]"), L"Color (APHA)");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"GELCONTENT"), L"Gel Content");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"5%SOLUTIONVISCOSITY[(]CPS[)]"), L"5% Solution viscosity(cps)");
				}

				return cleaned;
			}


			static std::wstring
				postprocess_item_unit(const std::wstring& item_unit, const std::wstring& category)
			{
				if (item_unit.length() > 30)
					return L"";

				auto cleaned = boost::regex_replace(item_unit, boost::wregex(L"/zm|Z/m|/zni"), L"nm");
				cleaned = boost::regex_replace(cleaned, boost::wregex(L"pl~l(.*)|PH"), L"pH");
				cleaned = boost::regex_replace(cleaned, boost::wregex(L"[-—]"), L"");

				if (category == L"ALBEMARLE") {
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"Wtl|W%|Wt&"), L"Wt&");
				}

				return cleaned;
			}

			static std::wstring
				postprocess_item_result(const std::wstring& item_result, const std::wstring& category)
			{
				if (item_result.length() > 30)
					return L"";

				auto cleaned = item_result;
				if (category == L"BASF_B") {
					cleaned = boost::regex_replace(cleaned, boost::wregex(L","), L".");
				}
				else if (category == L"DUKSAN") {
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"cass"), L"pass");
				}
				else if (category == L"ALBEMARLE") {
					cleaned = boost::regex_replace(cleaned, boost::wregex(L"c", boost::wregex::icase), L"0");
					cleaned = boost::regex_replace(cleaned, boost::wregex(L","), L".");
				}

				return cleaned;
			}
		};

		class court_document_recognizer : public recognizer
		{
		public:
			std::pair<std::wstring, int>
				recognize(const std::string& buffer, int languages, const std::string& secret) override
			{
				throw std::logic_error("not implmented");
			}

			std::pair<std::wstring, int>
				recognize(const std::string& buffer, const std::wstring& type, const std::string& secret) override
			{
				throw std::logic_error("not implmented");
			}

			std::unordered_map<std::wstring, std::vector<std::wstring>>
				recognize(const boost::filesystem::path& path, const std::string& secret) override
			{
				throw std::logic_error("not implmented");
			}

			std::unordered_map<std::wstring, std::vector<std::wstring>>
				recognize(const boost::filesystem::path& path,
				const std::wstring& class_name) override
			{
				const auto configuration = load_configuration(L"court");

				std::unordered_map<std::wstring, std::vector<std::unordered_map<std::wstring, std::vector<std::wstring>>>> fields;

				std::mutex locks;
				const auto fill_field = [&locks](std::unordered_map<std::wstring, std::vector<std::wstring>>& fields, const std::wstring& field,
					const std::vector<std::pair<cv::Rect, std::wstring>>& value) {
					locks.lock();
					if (!value.empty()) {
						std::vector<std::wstring> insert_value;
						std::transform(std::begin(value), std::end(value), std::back_inserter(insert_value), [](const std::pair<cv::Rect, std::wstring>& line) {
							return std::get<1>(line);
						});
						fields.emplace(std::make_pair(field, insert_value));
					}
					locks.unlock();
				};

				std::vector<std::wstring> files;
				if (boost::filesystem::is_directory(path)) {
					for (auto& entry : boost::filesystem::recursive_directory_iterator(path)) {
						const auto file = entry.path();
						const auto extension = boost::algorithm::to_lower_copy(file.extension().native());

						if (extension != L".png" && extension != L".jpg" && extension != L".tif" && extension != L".pdf")
							continue;

						files.emplace_back(boost::filesystem::absolute(file).native());
					}

					std::sort(std::begin(files), std::end(files), compareNat);
				}
				else {
					const auto extension = boost::algorithm::to_lower_copy(path.extension().native());

					if (extension != L".png" && extension != L".jpg" && extension != L".tif" && extension != L".pdf")
						return std::unordered_map<std::wstring, std::vector<std::wstring>>();

					files.emplace_back(boost::filesystem::absolute(path).native());
				}

				std::unordered_map<std::wstring, std::vector<std::wstring>> extracted_fields;

#ifdef LOG_USE_WOFSTREAM
				std::wofstream txt_file;
				txt_file.imbue(std::locale(std::locale::empty(), new std::codecvt_utf8<wchar_t, 0x10ffff, std::generate_header>));
				txt_file.open(__LOG_FILE_NAME__, std::wofstream::out | std::wofstream::app);
#endif

				try {
					// cv::parallel_for_(cv::Range(0, files.size()), [&](const cv::Range& range) {

					CComPtr<FREngine::IEngineLoader> loader;
					FREngine::IEnginePtr engine;
#if __USE_ENGINE__
					std::tie(loader, engine) = get_engine_object(configuration);
#endif
					// for (auto i = range.start; i < range.end; i++) {
					for (auto i = 0; i < files.size(); i++) {
						memory_reader memory_reader(boost::filesystem::absolute(files[i]), "");

						FREngine::IFRDocumentPtr document;
#if __USE_ENGINE__
						document = engine->CreateFRDocument();
						document->AddImageFileFromStream(&memory_reader, nullptr, nullptr, nullptr,
							boost::filesystem::path(files[i]).filename().native().c_str());

						auto page_preprocessing_params = engine->CreatePagePreprocessingParams();
						page_preprocessing_params->CorrectOrientation = ORIENTATION_VALUE;
						page_preprocessing_params->OrientationDetectionParams->put_OrientationDetectionMode(FREngine::OrientationDetectionModeEnum::ODM_Thorough);
						document->Preprocess(page_preprocessing_params, nullptr, nullptr, nullptr);

						if (document->Pages->Count < 1) {
							document->Close();
							continue;
						}
#endif

#ifdef LOG_USE_WOFSTREAM
						txt_file << L"-----------------------------------------------------" << std::endl;
						txt_file << L"File : " << files[i] << std::endl;
#endif

						if (class_name.empty()) {
#ifdef __USE_ENGINE__
							document->Close();
#endif
							continue;
						}

						const std::set<std::wstring> processed_class_names{
							L"document",
						};

						// 인식할 필요가 없는 문서는 pass
						if (processed_class_names.find(class_name) == processed_class_names.end()) {
							document->Close();
							continue;
						}

						std::vector<block> blocks;
						cv::Size image_size;
						std::tie(blocks, std::ignore, image_size) = recognize_document(engine, configuration, class_name, files[i], document);
						if (image_size.area() == 0)
							image_size = estimate_paper_size(blocks);

#if defined(GET_IMAGE_)
						auto temp_image = cv::imread("TEMP_GRAY.png");
#else
						cv::Mat temp_image;
#endif

#if __USE_ENGINE__
						document->Close();
#endif

						const auto keywords = get_keywords(configuration, keywords_, class_name);
						const auto searched_fields = search_fields(class_name, keywords, blocks);



						if (class_name == L"document") {
							auto court_name = extract_court_name(configuration, class_name, searched_fields, blocks, image_size);
							auto event_name = extract_event_name(configuration, class_name, searched_fields, blocks, image_size);
							auto creditor_name = extract_creditor_name(configuration, class_name, searched_fields, blocks, image_size);
							auto debtor_name = extract_debtor_name(configuration, class_name, searched_fields, blocks, image_size);
							auto debtor_other_name = extract_debtor_other_name(configuration, class_name, searched_fields, blocks, image_size);
							auto price_name = extract_price_name(configuration, class_name, searched_fields, blocks, image_size);
							auto judge_name = extract_judge_name(configuration, class_name, searched_fields, blocks, image_size);
							extract_extra_fields(configuration, class_name, searched_fields, blocks, creditor_name, debtor_name, debtor_other_name, image_size);

							fill_field(extracted_fields, L"COURT NAME", court_name);
							fill_field(extracted_fields, L"EVENT", event_name);
							fill_field(extracted_fields, L"CREDITOR", creditor_name);
							fill_field(extracted_fields, L"DEBTOR", debtor_name);
							fill_field(extracted_fields, L"DEBTOR_OTHER", debtor_other_name);
							fill_field(extracted_fields, L"PRICE", price_name);
							fill_field(extracted_fields, L"JUDGE", judge_name);

#if defined(GET_IMAGE_)
							const auto new_dir = OUTPUT_FOLDER + path.stem().native();
							boost::filesystem::create_directory(new_dir);

							saveArea(new_dir, L"COURT NAME", temp_image, court_name);
							saveArea(new_dir, L"EVENT", temp_image, event_name);
							saveArea(new_dir, L"CREDITOR", temp_image, creditor_name);
							saveArea(new_dir, L"DEBTOR", temp_image, debtor_name);
							saveArea(new_dir, L"DEBTOR_OTHER", temp_image, debtor_other_name);
							saveArea(new_dir, L"PRICE", temp_image, price_name);
							saveArea(new_dir, L"JUDGE", temp_image, judge_name);
#endif
						}



#ifdef LOG_USE_WOFSTREAM            // 문서단위 결과
						const auto text = to_wstring(blocks);
						txt_file << L"ALL : " << std::endl;
						txt_file << text << std::endl << std::endl;

						txt_file << L"- 파일 : " << files[i] << std::endl;
						txt_file << L"- 분류 : " << class_name << std::endl;

						txt_file << "- " << L"법원" << " : " << boost::algorithm::join(get_field_values(extracted_fields, L"COURT NAME"), L",") << std::endl;
						txt_file << "- " << L"사건" << " : " << boost::algorithm::join(get_field_values(extracted_fields, L"EVENT"), L",") << std::endl;
						txt_file << "- " << L"채권자" << " : " << boost::algorithm::join(get_field_values(extracted_fields, L"CREDITOR"), L",") << std::endl;
						txt_file << "- " << L"채무자" << " : " << boost::algorithm::join(get_field_values(extracted_fields, L"DEBTOR"), L",") << std::endl;
						txt_file << "- " << L"제3채무자" << " : " << boost::algorithm::join(get_field_values(extracted_fields, L"DEBTOR_OTHER"), L",") << std::endl;
						txt_file << "- " << L"청구금액" << " : " << boost::algorithm::join(get_field_values(extracted_fields, L"PRICE"), L",") << std::endl;
						txt_file << "- " << L"판사" << " : " << boost::algorithm::join(get_field_values(extracted_fields, L"JUDGE"), L",") << std::endl;
						txt_file.close();
#endif




						if (extracted_fields.empty())
							continue;

						if (fields.find(class_name) == fields.end())
							fields.emplace(class_name, std::vector<std::unordered_map<std::wstring, std::vector<std::wstring>>>());

						fields.at(class_name).emplace_back(extracted_fields);
					}

#if __USE_ENGINE__
					release_engine_object(std::make_pair(loader, engine));
#endif
					// } , std::stoi(configuration.at(L"engine").at(L"concurrency")));
				}
				catch (_com_error& e) {
					spdlog::get("recognizer")->error("exception : {} : ({} : {})", to_cp949(e.Description().GetBSTR()), __FILE__, __LINE__);
				}

				return extracted_fields;
			}

		private:
			static void saveArea(std::wstring dir_path, std::wstring keyword, cv::Mat image, std::vector<std::pair<cv::Rect, std::wstring>>& extracted_fields) {
				if (image.empty()) {
					return;
				}
				int i = 0;
				int value = 20;
				for (const auto& temp : extracted_fields) {
					auto rect = std::get<0>(temp);
					rect.x -= value / 2;
					rect.y -= value / 2;
					if (rect.x < 0) {
						rect.x = 0;
					}
					if (rect.y < 0) {
						rect.y = 0;
					}

					rect.width += value;
					rect.height += value;
					std::wstring save_file_path = fmt::format(L"{}{}{}_{:02d}{}", dir_path, L"\\", keyword, i, L".jpg");
					cv::imwrite(to_utf8(save_file_path).c_str(), image(rect));
					i++;
				}
			}


			static void drawArea(cv::Mat image, std::vector<std::pair<cv::Rect, std::wstring>>& extracted_fields) {
				if (image.empty()) {
					return;
				}
				for (const auto& temp : extracted_fields) {
					const auto& rect = std::get<0>(temp);
					cv::rectangle(image, rect, cv::Scalar(0, 0, 255), 1);
				}
			}
			static void insertConnectInfo(int index, std::vector<std::set<int>>& connect_info, std::set<int>& connect_list) {
				std::set<int>& current_set = connect_info[index];

				if (current_set.size() <= 0) {
					return;
				}

				connect_list.insert(current_set.begin(), current_set.end());
				current_set.erase(current_set.begin(), current_set.end());
				std::set<int> temp_list;
				for (auto iter = connect_list.begin(); iter != connect_list.end(); ++iter) {
					int new_index = *iter;
					if (connect_info[new_index].size() > 0) {
						insertConnectInfo(*iter, connect_info, temp_list);
					}
				}
				connect_list.insert(temp_list.begin(), temp_list.end());
			}

			static std::vector<std::vector<std::pair<cv::Rect, std::wstring>>>
				split_fields(std::vector<std::pair<cv::Rect, std::wstring>>& fields,
							 const std::function<bool(const std::pair<cv::Rect, std::wstring>& a, std::pair<cv::Rect, std::wstring>& b)>& condition) {
				std::vector<std::set<int>> connect_info;
				for (int i = 0; i < fields.size(); ++i) {
					std::set<int> temp;
					temp.insert(i);
					connect_info.push_back(temp);
				}

				for (int i = 0; i < fields.size(); ++i) {
					for (int j = 0; j < fields.size(); ++j) {
						if (i == j) {
							continue;
						}

						if (condition(fields[i], fields[j])) {
							connect_info[i].insert(j);
						}
					}
				}
				for (int i = 0; i < connect_info.size(); ++i) {
					std::set<int> connect_list;
					insertConnectInfo(i, connect_info, connect_list);
					connect_info[i] = connect_list;
				}

				std::vector<std::vector<std::pair<cv::Rect, std::wstring>>> ret;
				for (int i = fields.size() - 1; i >= 0; --i) {
					if (connect_info[i].size() > 0) {
						std::vector<std::pair<cv::Rect, std::wstring>> temp;
						for (auto iter = connect_info[i].begin(); iter != connect_info[i].end(); ++iter) {
							int index = *iter;
							temp.push_back(fields[index]);
						}
						ret.push_back(temp);
					}
				}
				return ret;
			}


			static std::vector<std::wstring>
				get_field_values(const std::unordered_map<std::wstring, std::vector<std::wstring>>& extracted_fields, const std::wstring& field_column) {
				for (const auto& field : extracted_fields) {
					if (std::get<0>(field) == field_column) {
						return std::get<1>(field);
					}
				}

				return std::vector<std::wstring>();
			}

			// 금액 [[
			static std::pair<cv::Rect, std::wstring>
				preprocess_price(const std::pair<cv::Rect, std::wstring>& price)
			{
				auto cleaned = std::get<1>(price);
				cleaned = boost::regex_replace(cleaned, boost::wregex(L":"), L"");

				boost::trim(cleaned);

				return std::make_pair(std::get<0>(price), cleaned);
			}

			static std::wstring
				postprocess_price(const std::wstring& price)
			{

				auto cleaned = boost::regex_replace(price, boost::wregex(L"[^0-9]"), L"");

				if (cleaned.length() < 5 || price.length() - cleaned.length() > 12) {
					return L"";
				}

				return price;
			}

			static std::wstring
				postprocess_judge(const std::wstring& price)
			{
				auto cleaned = boost::regex_replace(price, boost::wregex(L"[ ]"), L"");
				if (cleaned.length() > 6) {
					return L"";
				}

				return cleaned;
			}


			static std::vector<std::pair<cv::Rect, std::wstring>>
				extract_court_name(const configuration& configuration, const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
				const std::vector<block>& blocks, const cv::Size& image_size)
			{
				if (fields.find(L"COURT NAME") == fields.end())
					return std::vector<std::pair<cv::Rect, std::wstring>>();

				std::vector<std::pair<cv::Rect, std::wstring>> product_name;

				product_name = extract_field_values(fields.at(L"COURT NAME"), blocks,
					search_self_all,
					default_preprocess,
					create_extract_ori_function(configuration, L"courts", 2),
					default_postprocess);


				if (product_name.size() > 0) {
					product_name.erase(product_name.begin() + 1, product_name.end());
				}

				return product_name;
			}

			static std::vector<std::pair<cv::Rect, std::wstring>>
				extract_event_name(const configuration& configuration, const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
				const std::vector<block>& blocks, const cv::Size& image_size)
			{
				if (fields.find(L"EVENT") == fields.end())
					return std::vector<std::pair<cv::Rect, std::wstring>>();

				std::vector<std::pair<cv::Rect, std::wstring>> product_name;
				std::vector<std::pair<cv::Rect, std::wstring>> ret_items;

				product_name = extract_field_values_2(fields.at(L"EVENT"), blocks,
					{
						std::bind(filter_left_lines, std::placeholders::_1, std::placeholders::_2, 0.25, 5, 5, false),
						std::bind(filter_search, std::placeholders::_1, std::placeholders::_2)
					},
					std::bind(find_right_lines, std::placeholders::_1, std::placeholders::_2, 0.25, 0.0, 100, false),
					default_preprocess,
					default_extract,
					default_postprocess);

				if (product_name.empty()) {
					product_name = extract_field_values_2(fields.at(L"EVENT"), blocks,
						{
							std::bind(filter_left_lines, std::placeholders::_1, std::placeholders::_2, 0.25, 5, 5, false),
							std::bind(filter_search_2, std::placeholders::_1, std::placeholders::_2)
						},
						search_self,
						default_preprocess,
						default_extract,
						default_postprocess);
				}



				auto& split_product_name = split_fields(product_name, [](const std::pair<cv::Rect, std::wstring>& a, const std::pair<cv::Rect, std::wstring>& b) {
					auto a_expand = cv::Rect(0, a.first.y, std::numeric_limits<int>::max(), a.first.height);
					return (a_expand & b.first).height > std::min(a.first.height, b.first.height) * 0.25;
				});

				if (split_product_name.size() > 0) {
					for (const auto& split : split_product_name) {
						if (split.size() > 0) {
							std::pair<cv::Rect, std::wstring> temp = split[0];
							temp = std::accumulate(std::next(std::begin(split)), std::end(split), temp,
								[](const std::pair<cv::Rect, std::wstring>& pre, const std::pair<cv::Rect, std::wstring>& item) {
								return std::make_pair(pre.first | item.first, fmt::format(L"{} {}", pre.second, item.second));
							});
							ret_items.push_back(temp);
						}
					}
				}

				return ret_items;
			}

			static std::vector<std::pair<cv::Rect, std::wstring>>
				extract_creditor_name(const configuration& configuration, const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
				const std::vector<block>& blocks, const cv::Size& image_size)
			{
				if (fields.find(L"CREDITOR") == fields.end())
					return std::vector<std::pair<cv::Rect, std::wstring>>();

				std::vector<std::pair<cv::Rect, std::wstring>> product_name;
				std::vector<std::pair<cv::Rect, std::wstring>> ret_items;

				product_name = extract_field_values_2(fields.at(L"CREDITOR"), blocks,
					{	std::bind(filter_left_lines, std::placeholders::_1, std::placeholders::_2, 0.25, 5, 5, false) ,
						std::bind(filter_search, std::placeholders::_1, std::placeholders::_2) },
					std::bind(find_right_lines, std::placeholders::_1, std::placeholders::_2, 0.25, 0.0, 100, false),
					default_preprocess,
					default_extract,
					default_postprocess);

				if (product_name.empty()) {
					product_name = extract_field_values_2(fields.at(L"CREDITOR"), blocks,
					{
						std::bind(filter_left_lines, std::placeholders::_1, std::placeholders::_2, 0.25, 5, 5, false),
						std::bind(filter_search_2, std::placeholders::_1, std::placeholders::_2)
					},
					search_self,
					default_preprocess,
					default_extract,
					default_postprocess);
				}



				auto& split_product_name = split_fields(product_name, [](const std::pair<cv::Rect, std::wstring>& a, const std::pair<cv::Rect, std::wstring>& b) {
					auto a_expand = cv::Rect(0, a.first.y, std::numeric_limits<int>::max(), a.first.height);
					return (a_expand & b.first).height > std::min(a.first.height, b.first.height) * 0.25;
				});

				if (split_product_name.size() > 0) {
					for (const auto& split : split_product_name) {
						if (split.size() > 0) {
							std::pair<cv::Rect, std::wstring> temp = split[0];
							temp = std::accumulate(std::next(std::begin(split)), std::end(split), temp,
								[](const std::pair<cv::Rect, std::wstring>& pre, const std::pair<cv::Rect, std::wstring>& item) {
								return std::make_pair(pre.first | item.first, fmt::format(L"{} {}", pre.second, item.second));
							});
							ret_items.push_back(temp);
						}
					}
				}

				if (ret_items.size() > 0) {
					std::sort(std::begin(ret_items), std::end(ret_items),
						[](const std::pair<cv::Rect, std::wstring>& a, const std::pair<cv::Rect, std::wstring>& b) {
						auto a_temp = boost::regex_replace(std::get<1>(a), boost::wregex(L"[^0-9]"), L"");
						auto b_temp = boost::regex_replace(std::get<1>(b), boost::wregex(L"[^0-9]"), L"");

						if (a_temp.length() == b_temp.length()) {
							return to_rect(a).y < to_rect(b).y;
						}
						else {
							return a_temp.length() > b_temp.length();
						}
					});
					ret_items.erase(ret_items.begin() + 1, ret_items.end());
				}

				return ret_items;
			}

			static std::vector<std::pair<cv::Rect, std::wstring>>
				extract_debtor_other_name(const configuration& configuration, const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
				const std::vector<block>& blocks, const cv::Size& image_size)
			{
				if (fields.find(L"DEBTOR_OTHER") == fields.end())
					return std::vector<std::pair<cv::Rect, std::wstring>>();

				std::vector<std::pair<cv::Rect, std::wstring>> product_name;
				std::vector<std::pair<cv::Rect, std::wstring>> ret_items;

				product_name = extract_field_values_2(fields.at(L"DEBTOR_OTHER"), blocks,
					{	std::bind(filter_left_lines, std::placeholders::_1, std::placeholders::_2, 0.25, 5, 5, false),
						std::bind(filter_search, std::placeholders::_1, std::placeholders::_2)
					},
					std::bind(find_right_lines, std::placeholders::_1, std::placeholders::_2, 0.25, 0.0, 100, false),
					default_preprocess,
					default_extract,
					default_postprocess);

				if (product_name.empty()) {
					product_name = extract_field_values_2(fields.at(L"DEBTOR_OTHER"), blocks,
					{
						std::bind(filter_left_lines, std::placeholders::_1, std::placeholders::_2, 0.25, 5, 5, false),
						std::bind(filter_search_2, std::placeholders::_1, std::placeholders::_2)
					},
					search_self,
					default_preprocess,
					default_extract,
					default_postprocess);
				}


				auto& split_product_name = split_fields(product_name, [](const std::pair<cv::Rect, std::wstring>& a, const std::pair<cv::Rect, std::wstring>& b) {
					auto a_expand = cv::Rect(0, a.first.y, std::numeric_limits<int>::max(), a.first.height);
					return (a_expand & b.first).height > std::min(a.first.height, b.first.height) * 0.25;
				});

				if (split_product_name.size() > 0) {
					for (const auto& split : split_product_name) {
						if (split.size() > 0) {
							std::pair<cv::Rect, std::wstring> temp = split[0];
							temp = std::accumulate(std::next(std::begin(split)), std::end(split), temp,
								[](const std::pair<cv::Rect, std::wstring>& pre, const std::pair<cv::Rect, std::wstring>& item) {
								return std::make_pair(pre.first | item.first, fmt::format(L"{} {}", pre.second, item.second));
							});
							ret_items.push_back(temp);
						}
					}
				}

				if (ret_items.size() > 0) {
					std::sort(std::begin(ret_items), std::end(ret_items),
						[](const std::pair<cv::Rect, std::wstring>& a, const std::pair<cv::Rect, std::wstring>& b) {
						return to_rect(a).y < to_rect(b).y;
					});
					ret_items.erase(ret_items.begin() + 1, ret_items.end());
				}

				return ret_items;
			}


			static std::vector<std::pair<cv::Rect, std::wstring>>
				extract_debtor_name(const configuration& configuration, const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
				const std::vector<block>& blocks, const cv::Size& image_size)
			{
				if (fields.find(L"DEBTOR") == fields.end())
					return std::vector<std::pair<cv::Rect, std::wstring>>();

				std::vector<std::pair<cv::Rect, std::wstring>> product_name;
				std::vector<std::pair<cv::Rect, std::wstring>> ret_items;

				product_name = extract_field_values_2(fields.at(L"DEBTOR"), blocks,
				{
					std::bind(filter_left_lines, std::placeholders::_1, std::placeholders::_2, 0.25, 5, 5, false),
					std::bind(filter_search, std::placeholders::_1, std::placeholders::_2)
				},
				std::bind(find_right_lines, std::placeholders::_1, std::placeholders::_2, 0.25, 0.0, 100, false),
				default_preprocess,
				default_extract,
				default_postprocess);

				if (product_name.empty()) {
					product_name = extract_field_values_2(fields.at(L"DEBTOR"), blocks,
					{
						std::bind(filter_left_lines, std::placeholders::_1, std::placeholders::_2, 0.25, 5, 5, false),
						std::bind(filter_search_2, std::placeholders::_1, std::placeholders::_2)
					},
					search_self,
					default_preprocess,
					default_extract,
					default_postprocess);
				}


				auto& split_product_name = split_fields(product_name, [](const std::pair<cv::Rect, std::wstring>& a, const std::pair<cv::Rect, std::wstring>& b) {
					auto a_expand = cv::Rect(0, a.first.y, std::numeric_limits<int>::max(), a.first.height);
					return (a_expand & b.first).height > std::min(a.first.height, b.first.height) * 0.25;
				});

				if (split_product_name.size() > 0) {
					for (const auto& split : split_product_name) {
						if (split.size() > 0) {
							std::pair<cv::Rect, std::wstring> temp = split[0];
							temp = std::accumulate(std::next(std::begin(split)), std::end(split), temp,
								[](const std::pair<cv::Rect, std::wstring>& pre, const std::pair<cv::Rect, std::wstring>& item) {
								return std::make_pair(pre.first | item.first, fmt::format(L"{} {}", pre.second, item.second));
							});
							ret_items.push_back(temp);
						}
					}
				}

				if (ret_items.size() > 0) {
					std::sort(std::begin(ret_items), std::end(ret_items),
						[](const std::pair<cv::Rect, std::wstring>& a, const std::pair<cv::Rect, std::wstring>& b) {
						return to_rect(a).y < to_rect(b).y;
					});
					ret_items.erase(ret_items.begin() + 1, ret_items.end());
				}

				return ret_items;

			}

			static std::vector<std::pair<cv::Rect, std::wstring>>
				extract_price_name(const configuration& configuration, const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
				const std::vector<block>& blocks, const cv::Size& image_size)
			{
				if (fields.find(L"PRICE") == fields.end())
					return std::vector<std::pair<cv::Rect, std::wstring>>();

				std::vector<std::pair<cv::Rect, std::wstring>> product_name;

				product_name = extract_field_values(fields.at(L"PRICE"), blocks,
					std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
					court_document_recognizer::preprocess_price,
					create_extract_function(L"([금]{0,1} ?.* ?[원])"),
					court_document_recognizer::postprocess_price);

				if (product_name.empty()) {
					product_name = extract_field_values(fields.at(L"PRICE"), blocks,
						search_self,
						court_document_recognizer::preprocess_price,
						create_extract_function(L"([금]{0,1} ?.* ?[원])"),
						court_document_recognizer::postprocess_price);
				}

				if (product_name.empty()) {
					product_name = extract_field_values(fields.at(L"PRICE"), blocks,
						std::bind(find_down_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 8.0, 100, true),
						court_document_recognizer::preprocess_price,
						create_extract_function(L"([금]{0,1} ?.* ?[원])"),
						court_document_recognizer::postprocess_price);

					std::sort(std::begin(product_name), std::end(product_name),
						[](const std::pair<cv::Rect, std::wstring>& a, const std::pair<cv::Rect, std::wstring>& b) {
						auto a_num = std::stoi(boost::regex_replace(std::get<1>(a), boost::wregex(L"[^0-9]"), L""));
						auto b_num = std::stoi(boost::regex_replace(std::get<1>(b), boost::wregex(L"[^0-9]"), L""));
						return a_num > b_num;
					});
				}

				if (product_name.empty()) {
					product_name = extract_field_values(std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>{
						std::make_tuple(cv::Rect(), L"", cv::Range())
						}, blocks,
						default_search,
						court_document_recognizer::preprocess_price,
						create_extract_function(L"([금]{0,1} ?.* ?[원])"),
						court_document_recognizer::postprocess_price);

					std::sort(std::begin(product_name), std::end(product_name),
						[](const std::pair<cv::Rect, std::wstring>& a, const std::pair<cv::Rect, std::wstring>& b) {
						auto a_num = std::stoi(boost::regex_replace(std::get<1>(a), boost::wregex(L"[^0-9]"), L""));
						auto b_num = std::stoi(boost::regex_replace(std::get<1>(b), boost::wregex(L"[^0-9]"), L""));
						return a_num > b_num;
					});
				}

				if (product_name.size() > 0) {
					product_name.erase(product_name.begin() + 1, product_name.end());
				}

				return product_name;
			}

			static std::vector<std::pair<cv::Rect, std::wstring>>
				extract_judge_name(const configuration& configuration, const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
				const std::vector<block>& blocks, const cv::Size& image_size)
			{
				if (fields.find(L"JUDGE") == fields.end())
					return std::vector<std::pair<cv::Rect, std::wstring>>();

				std::vector<std::pair<cv::Rect, std::wstring>> product_name;

				product_name = extract_field_values(fields.at(L"JUDGE"), blocks,
					std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
					default_preprocess,
					default_extract,
					court_document_recognizer::postprocess_judge);

				if (product_name.empty()) {
					product_name = extract_field_values(fields.at(L"JUDGE"), blocks,
						search_self,
						default_preprocess,
						default_extract,
						court_document_recognizer::postprocess_judge);
				}

				//if (product_name.size() > 0) {
				//	product_name.erase(product_name.begin() + 1, product_name.end());
				//}

				return product_name;
			}

			static std::tuple<cv::Rect, cv::Rect, cv::Rect>
				createExtraRects(std::vector<std::pair<cv::Rect, std::wstring>>& creditor,
								 std::vector<std::pair<cv::Rect, std::wstring>>& debtors,
								 std::vector<std::pair<cv::Rect, std::wstring>>& debtor_others)
			{
				cv::Rect extra_creditor(0, 0, 0 ,0);
				cv::Rect extra_debtors(0, 0, 0, 0);
				cv::Rect extra_debtor_others(0, 0, 0, 0);

				if (creditor.size() > 0) {
					std::sort(std::begin(creditor), std::end(creditor),
						[](const std::pair<cv::Rect, std::wstring>& a, const std::pair<cv::Rect, std::wstring>& b) {
						return to_rect(a).y < to_rect(b).y;
					});

				}

				if (debtors.size() > 0) {
					std::sort(std::begin(debtors), std::end(debtors),
						[](const std::pair<cv::Rect, std::wstring>& a, const std::pair<cv::Rect, std::wstring>& b) {
						return to_rect(a).y < to_rect(b).y;
					});
				}

				if (debtor_others.size() > 0) {
					std::sort(std::begin(debtor_others), std::end(debtor_others),
						[](const std::pair<cv::Rect, std::wstring>& a, const std::pair<cv::Rect, std::wstring>& b) {
						return to_rect(a).y < to_rect(b).y;
					});
				}


				if (creditor.size() > 0) {
					cv::Rect cre_rect = creditor.front().first;
					if (debtors.size() > 0) {
						cv::Rect deb_rect = debtors.back().first;
						extra_creditor = cre_rect;
						extra_creditor.height = deb_rect.y - extra_creditor.y;
					}
					else {
						extra_creditor = cre_rect;
						extra_creditor.height = std::numeric_limits<int>::max();
					}
				}

				if (debtors.size() > 0) {
					cv::Rect deb_rect = debtors.front().first;
					if (debtor_others.size() > 0) {
						cv::Rect deb_other_rect = debtor_others.back().first;
						extra_debtors = deb_rect;
						extra_debtors.height = deb_other_rect.y - extra_debtors.y;
					}
					else {
						extra_debtors = deb_rect;
						extra_debtors.height = std::numeric_limits<int>::max();
					}
				}

				if (debtor_others.size() > 0) {
					cv::Rect deb_other_rect = debtor_others.back().first;
					extra_debtor_others = deb_other_rect;
					extra_debtor_others.height = std::numeric_limits<int>::max();
				}

				return std::make_tuple(extra_creditor, extra_debtors, extra_debtor_others);
			}

			static void
				extract_extra_field(std::vector<std::pair<cv::Rect, std::wstring>>& fields, std::vector<std::pair<cv::Rect, std::wstring>>& extra) {
				if (extra.size() <= 0) {
					return;
				}
				std::sort(std::begin(extra), std::end(extra),
					[](const std::pair<cv::Rect, std::wstring>& a, const std::pair<cv::Rect, std::wstring>& b) {
					return to_rect(a).y < to_rect(b).y;
				});
				std::vector<std::pair<cv::Rect, std::wstring>> insert_list;
				for (const auto& field : fields) {
					cv::Rect field_rect = std::get<0>(field);
					int field_bottom = field_rect.y + field_rect.height;
					if (!boost::regex_search(std::get<1>(field), boost::wregex(L"^.?[1|ㅣ]", boost::regex_constants::icase))) {
						continue;
					}
					int pre_number = 1;
					for (const auto& extra_field : extra) {
						cv::Rect extra_rect = std::get<0>(extra_field);
						std::wstring extra_str = std::get<1>(extra_field);
						if (field_bottom > extra_rect.y) {
							continue;
						}
						auto clean = boost::regex_replace(extra_str, boost::wregex(L"[^0-9]", boost::regex_constants::icase), L"");
						if (clean.length() > 0) {
							std::wstring temp;
							temp += clean[0];
							if (std::stoi(temp) == pre_number + 1) {
								insert_list.push_back(extra_field);
								pre_number++;
							}
							else {
								break;
							}
						}
					}
				}
				fields.insert(fields.end(), insert_list.begin(), insert_list.end());
				std::sort(std::begin(fields), std::end(fields),
					[](const std::pair<cv::Rect, std::wstring>& a, const std::pair<cv::Rect, std::wstring>& b) {
					return to_rect(a).y < to_rect(b).y;
				});
			}

			static void
				extract_extra_fields(const configuration& configuration, const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
				const std::vector<block>& blocks,
				std::vector<std::pair<cv::Rect, std::wstring>>& creditor,
				std::vector<std::pair<cv::Rect, std::wstring>>& debtors,
				std::vector<std::pair<cv::Rect, std::wstring>>& debtor_others,
				const cv::Size& image_size)
			{
				auto rects = createExtraRects(creditor, debtors, debtor_others);
				auto extra_creditors = find_area_lines(std::make_tuple(std::get<0>(rects), L"", cv::Range(0, 0)), blocks, 0.8);
				auto extra_debtors = find_area_lines(std::make_tuple(std::get<1>(rects), L"", cv::Range(0, 0)), blocks, 0.8);
				auto extra_debtor_others = find_area_lines(std::make_tuple(std::get<2>(rects), L"", cv::Range(0, 0)), blocks, 0.8);

				boost::remove_erase_if(extra_creditors, [](const std::pair<cv::Rect, std::wstring>& name) {
					return !boost::regex_search(std::get<1>(name), boost::wregex(L"^.?[0-9]", boost::regex_constants::icase)); });
				boost::remove_erase_if(extra_debtors, [](const std::pair<cv::Rect, std::wstring>& name) {
					return !boost::regex_search(std::get<1>(name), boost::wregex(L"^.?[0-9]", boost::regex_constants::icase)); });
				boost::remove_erase_if(extra_debtor_others, [](const std::pair<cv::Rect, std::wstring>& name) {
					return !boost::regex_search(std::get<1>(name), boost::wregex(L"^.?[0-9]", boost::regex_constants::icase)); });

				extract_extra_field(creditor, extra_creditors);
				extract_extra_field(debtors, extra_debtors);
				extract_extra_field(debtor_others, extra_debtor_others);
			}

		};

		class account_recognizer : public recognizer
		{
		public:
			std::pair<std::wstring, int>
				recognize(const std::string& buffer, int languages, const std::string& secret) override
			{
				throw std::logic_error("not implmented");
			}

			std::pair<std::wstring, int>
				recognize(const std::string& buffer, const std::wstring& type, const std::string& secret) override
			{
				throw std::logic_error("not implmented");
			}

			std::unordered_map<std::wstring, std::vector<std::wstring>>
				recognize(const boost::filesystem::path& path, const std::string& secret) override
			{
				const auto configuration = load_configuration(L"account");

				std::unordered_map<std::wstring, std::vector<std::unordered_map<std::wstring, std::vector<std::wstring>>>> fields;

				std::mutex locks;
				const auto fill_field = [&locks](std::unordered_map<std::wstring, std::vector<std::wstring>>& fields, const std::wstring& field,
					const std::vector<std::wstring>& value) {
					locks.lock();
					if (!value.empty())
						fields.emplace(std::make_pair(field, value));
					locks.unlock();
				};

				std::vector<std::wstring> files;
				if (boost::filesystem::is_directory(path)) {
					for (auto& entry : boost::filesystem::recursive_directory_iterator(path)) {
						const auto file = entry.path();
						const auto extension = boost::algorithm::to_lower_copy(file.extension().native());

						if (extension != L".png" && extension != L".jpg" && extension != L"tif" && extension != L".pdf")
							continue;

						files.emplace_back(boost::filesystem::absolute(file).native());
					}

					std::sort(std::begin(files), std::end(files), compareNat);
				}
				else {
					const auto extension = boost::algorithm::to_lower_copy(path.extension().native());

					if (extension != L".png" && extension != L".jpg" && extension != L".tif" && extension != L".pdf")
						return std::unordered_map<std::wstring, std::vector<std::wstring>>();

					files.emplace_back(boost::filesystem::absolute(path).native());
				}

				std::unordered_map<std::wstring, std::vector<std::wstring>> extracted_fields;

#ifdef LOG_USE_WOFSTREAM
				std::wofstream txt_file;
				txt_file.imbue(std::locale(std::locale::empty(), new std::codecvt_utf8<wchar_t, 0x10ffff, std::generate_header>));
				txt_file.open(__LOG_FILE_NAME__, std::wofstream::out | std::wofstream::app);
#endif
				try {
					// cv::parallel_for_(cv::Range(0, files.size()), [&](const cv::Range& range) {

					CComPtr<FREngine::IEngineLoader> loader;
					FREngine::IEnginePtr engine;
#ifdef __USE_ENGINE__
					std::tie(loader, engine) = get_engine_object(configuration);
#endif

					// for (auto i = range.start; i < range.end; i++) {
					for (auto i = 0; i < files.size(); i++) {
						memory_reader memory_reader(boost::filesystem::absolute(files[i]), secret);

						FREngine::IFRDocumentPtr document;
#ifdef __USE_ENGINE__
						document = engine->CreateFRDocument();
						document->AddImageFileFromStream(&memory_reader, nullptr, nullptr, nullptr,
							boost::filesystem::path(files[i]).filename().native().c_str());

						auto page_preprocessing_params = engine->CreatePagePreprocessingParams();
						page_preprocessing_params->CorrectOrientation = VARIANT_TRUE;
						page_preprocessing_params->OrientationDetectionParams->put_OrientationDetectionMode(FREngine::OrientationDetectionModeEnum::ODM_Thorough);
						document->Preprocess(page_preprocessing_params, nullptr, nullptr, nullptr);

						if (document->Pages->Count < 1) {
							document->Close();
							continue;
						}
#endif

#ifdef LOG_USE_WOFSTREAM
						txt_file << L"-----------------------------------------------------" << std::endl;
						txt_file << L"File : " << files[i] << std::endl;
#endif

						const std::wstring class_name = L"ACCOUNT";
						if (class_name.empty()) {
#ifdef __USE_ENGINE__
							document->Close();
#endif
							continue;
						}

						const std::set<std::wstring> processed_class_names{
							L"ACCOUNT",
						};

						// 인식할 필요가 없는 문서는 pass
						if (processed_class_names.find(class_name) == processed_class_names.end()) {
							document->Close();
							continue;
						}

						std::vector<block> blocks_ko;
						cv::Size image_size;
						std::tie(blocks_ko, std::ignore, image_size) = recognize_document_with_language(engine, configuration, class_name, files[i], document, L"ko");
						if (image_size.area() == 0)
							image_size = estimate_paper_size(blocks_ko);

						// Extract owner_name, bank_name [[
						auto keywords = get_keywords(configuration, keywords_, class_name);
						auto searched_fields = search_fields(class_name, keywords, blocks_ko);

						auto account_bank_owner = extract_account_owner_name(configuration, class_name, searched_fields, blocks_ko, image_size);
						auto account_bank_name = extract_account_bank_name(configuration, class_name, searched_fields, blocks_ko, image_size);

						fill_field(extracted_fields, L"ACCOUNT BANK OWNER", std::vector<std::wstring>{ account_bank_owner });
						fill_field(extracted_fields, L"ACCOUNT BANK NAME", std::vector<std::wstring>{ account_bank_name });
						// ]]

						std::vector<block> blocks_digit;
						std::tie(blocks_digit, std::ignore, image_size) = recognize_document_with_language(engine, configuration, class_name, files[i], document, L"digit");

						// Extract number [[
						auto account_number_in_ko = extract_account_number(configuration, class_name, searched_fields, blocks_ko, image_size);
						auto account_number_in_digit = extract_account_number(configuration, class_name, searched_fields, blocks_digit, image_size);

						fill_field(extracted_fields, L"ACCOUNT NUMBER", std::vector<std::wstring>{ account_number_in_ko.length() > account_number_in_digit.length() ? account_number_in_ko : account_number_in_digit });
						// ]]
#ifdef __USE_ENGINE__
						document->Close();
#endif

#if defined(_DEBUG)
						const auto image = debug_;
#endif

#ifdef LOG_USE_WOFSTREAM            // 문서단위 결과
						auto text = to_wstring(blocks_ko);
						txt_file << L"----------------------- ALL KO -----------------------" << std::endl;
						txt_file << text << std::endl << std::endl;
						text = to_wstring(blocks_digit);
						txt_file << L"----------------------- ALL DIGIT -----------------------" << std::endl;
						txt_file << text << std::endl << std::endl;

						txt_file << L"- 파일 : " << files[i] << std::endl;
						txt_file << L"- 분류 : " << class_name << std::endl;

						txt_file << "- " << L"예금주" << " : " << boost::algorithm::join(get_field_values(extracted_fields, L"ACCOUNT BANK OWNER"), L",") << std::endl;
						txt_file << "- " << L"은행명" << " : " << boost::algorithm::join(get_field_values(extracted_fields, L"ACCOUNT BANK NAME"), L",") << std::endl;
						txt_file << "- " << L"계좌번호" << " : " << boost::algorithm::join(get_field_values(extracted_fields, L"ACCOUNT NUMBER"), L",") << std::endl;
						txt_file.close();
#endif

						if (extracted_fields.empty())
							continue;

						if (fields.find(class_name) == fields.end())
							fields.emplace(class_name, std::vector<std::unordered_map<std::wstring, std::vector<std::wstring>>>());

						fields.at(class_name).emplace_back(extracted_fields);
					}

#ifdef __USE_ENGINE__
					release_engine_object(std::make_pair(loader, engine));
#endif
					// } , std::stoi(configuration.at(L"engine").at(L"concurrency")));
				}
				catch (_com_error& e) {
					spdlog::get("recognizer")->error("exception : {} : ({} : {})", to_cp949(e.Description().GetBSTR()), __FILE__, __LINE__);
				}

				return extracted_fields;
			}

			std::unordered_map<std::wstring, std::vector<std::wstring>>
				recognize(const boost::filesystem::path& path,
				const std::wstring& class_name) override
			{
				throw std::logic_error("not implmented");
			}

		private:
			static std::vector<std::wstring>
				get_field_values(const std::unordered_map<std::wstring, std::vector<std::wstring>>& extracted_fields, const std::wstring& field_column) {
				for (const auto& field : extracted_fields) {
					if (std::get<0>(field) == field_column) {
						return std::get<1>(field);
					}
				}

				return std::vector<std::wstring>();
			}

			// ACCOUNT BANK NAME [[
			static std::pair<cv::Rect, std::wstring>
				preprocess_account_bank_name(const std::pair<cv::Rect, std::wstring>& bank_name)
			{
				auto cleaned = std::get<1>(bank_name);

				return std::make_pair(std::get<0>(bank_name), cleaned);
			}

			static std::wstring
				extract_account_bank_name(const configuration& configuration, const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
				const std::vector<block>& blocks, const cv::Size& image_size)
			{
				auto bank_names = extract_field_values(std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>{
					std::make_tuple(cv::Rect(), L"", cv::Range())
				}, blocks,
				default_search,
				default_preprocess,
				create_extract_function(configuration, L"banks", 2),
				postprocess_account_bank_name);

				return bank_names.size() > 0 ? std::get<1>(bank_names[0]) : L"";
			}

			static std::wstring
				postprocess_account_bank_name(const std::wstring& bank_name)
			{
				auto cleaned = bank_name;

				return cleaned;
			}
			// ACCOUNT BANK NAME ]]

			// ACCOUNT OWNER NAME [[
			static std::pair<cv::Rect, std::wstring>
				preprocess_account_owner_name(const std::pair<cv::Rect, std::wstring>& owner_name)
			{
				auto cleaned = std::get<1>(owner_name);

				return std::make_pair(std::get<0>(owner_name), cleaned);
			}

			static std::wstring
				extract_account_owner_name(const configuration& configuration, const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
				const std::vector<block>& blocks, const cv::Size& image_size)
			{
				auto owner_names = extract_field_values(std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>{
					std::make_tuple(cv::Rect(), L"", cv::Range())
				}, blocks,
				default_search,
				default_preprocess,
				create_extract_function(L"(.*) ?[님림]"),
				postprocess_account_owner_name);

				return owner_names.size() > 0 ? std::get<1>(owner_names[0]) : L"";
			}

			static std::wstring
				postprocess_account_owner_name(const std::wstring& owner_name)
			{
				auto cleaned = owner_name;
				if (boost::regex_search(cleaned, boost::wregex(L"신한|은행|고객")))
					return L"";

				return cleaned;
			}
			// ACCOUNT OWNER NAME ]]

			// ACCOUNT NUMBER [[
			static std::pair<cv::Rect, std::wstring>
				preprocess_account_number(const std::pair<cv::Rect, std::wstring>& number)
			{
				auto cleaned = boost::regex_replace(std::get<1>(number), boost::wregex(L"[ᅳ—~]"), L"-");

				return std::make_pair(std::get<0>(number), cleaned);
			}

			static std::wstring
				extract_account_number(const configuration& configuration, const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
				const std::vector<block>& blocks, const cv::Size& image_size)
			{
				auto numbers = extract_field_values(std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>{
					std::make_tuple(cv::Rect(), L"", cv::Range())
				}, blocks,
				default_search,
				preprocess_account_number,
				create_extract_function(L"([0-9]{2,6} ?- ?[0-9]{2,6} ?-? ?(?:[0-9]{2,6} ?- ?)?[0-9]{2,6})"),
				postprocess_account_number);

				if (numbers.size() > 0) {
					std::wstring longest_number;
					for (int i = 0; i < numbers.size(); i++) {
						if (i == 0) {
							longest_number = std::get<1>(numbers[i]);
						}
						else {
							if (std::get<1>(numbers[i]).size() > longest_number.size()) {
								longest_number = std::get<1>(numbers[i]);
							}
						}
					}
				}

				return numbers.size() > 0 ? std::get<1>(numbers[0]) : L"";
			}

			static std::wstring
				postprocess_account_number(const std::wstring& number)
			{
				auto cleaned = boost::regex_replace(number, boost::wregex(L" "), L"");

				auto without_dash = boost::regex_replace(cleaned, boost::wregex(L"-"), L"");
				if (without_dash.size() < 11 || without_dash.size() > 14)
					return L"";

				auto only_dash = boost::regex_replace(cleaned, boost::wregex(L"[^-]"), L"");
				if (only_dash.size() < 2)
					return L"";

				if (boost::regex_search(cleaned, boost::wregex(L"080-023-01")))
					return L"";

				if (boost::regex_search(cleaned, boost::wregex(L"80-365-50")))
					return L"";

				int first_year = 0;
				if (cleaned.size() > 4) {
					try {
						first_year = std::stoi(cleaned.substr(0, 4));
					}
					catch (std::exception& e) {
						first_year = 0;
					}

					if (first_year >= 1990 && first_year <= 2025) {
						return L"";
					}
				}

				return cleaned;
			}
			// ACCOUNT NUMBER ]]

			// UTILS [[
			static bool isHorizontalLine(const cv::Rect& rect_1, const cv::Rect& rect_2, float hitting_threshold) {
				auto basis_rect = rect_1;
				basis_rect.x = 0;
				basis_rect.width = std::numeric_limits<int>::max();

				const auto collision = basis_rect & rect_2;

				if (collision.height >= std::min(basis_rect.height, rect_2.height) * hitting_threshold) {
					return true;
				}
				return false;
			}

			static cv::Rect field_to_rect(const std::tuple<cv::Rect, std::wstring, cv::Range>& field) {
				cv::Rect ret = std::get<0>(field);
				cv::Range range = std::get<2>(field);
				int len = std::get<1>(field).length();
				if (len <= 0) {
					return ret;
				}
				int one_char_width = ret.width / len;
				ret.x = one_char_width * range.start;
				ret.width = one_char_width * range.size();
				return ret;
			}

			static std::pair<cv::Rect, std::wstring> get_horizontal_line(std::vector<std::pair<cv::Rect, std::wstring>>& extracted, cv::Rect rect, float hitting_threshold = 0.8) {
				for (auto& ex_i : extracted) {
					if (isHorizontalLine(rect, std::get<0>(ex_i), hitting_threshold)) {
						return ex_i;
					}
				}
				return std::make_pair(cv::Rect(), L"");
			}

			static std::vector<std::pair<cv::Rect, std::wstring>> get_horizontal_lines(std::vector<std::pair<cv::Rect, std::wstring>>& extracted, cv::Rect rect, float hitting_threshold = 0.8) {
				std::vector<std::pair<cv::Rect, std::wstring>> ret;
				for (auto& ex_i : extracted) {
					if (isHorizontalLine(rect, std::get<0>(ex_i), hitting_threshold)) {
						ret.push_back(ex_i);
					}
				}
				return ret;
			}

			static void remove_not_horizontal(std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>& a,
				std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>& b) {
				for (int i = a.size() - 1; i >= 0; --i) {
					cv::Rect item_rect = std::get<0>(a[i]);
					bool flag = false;
					for (int j = b.size() - 1; j >= 0; --j) {
						cv::Rect result_rect = std::get<0>(b[j]);
						if (isHorizontalLine(item_rect, result_rect, 0.8f)) {
							flag = true;
							break;
						}
					}
					if (!flag) {
						a.erase(a.begin() + i);
					}
				}
			}

			static std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>> line_field_to_word_field(const std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>& field,
				const std::vector<block>& blocks) {
				std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>> ret;
				for (const auto& line_field : field) {
					for (const auto& block : blocks) {
						for (const auto& line : block) {
							if (to_rect(line) == std::get<0>(line_field)) {
								int char_size = 0;
								for (const auto& word : line) {
									char_size += word.size();
								}
								char_size += (line.size() - 1);
								std::wstring line_str = std::get<1>(line_field);
								int line_length = line_str.length();
								float ratio = char_size / (float)line_length;
								cv::Range range = std::get<2>(line_field);
								int field_start = range.start * ratio;
								int field_end = range.end * ratio;

								std::vector<bool> temp_counts(line.size(), false);

								int start_index = 0;
								for (int i = 0; i < line.size(); ++i) {
									const auto& word = line[i];
									int end_index = start_index + word.size();
									int gap = std::min(field_end, end_index) - std::max(field_start, start_index);
									if (gap > word.size() * 0.8) {
										temp_counts[i] = true;
									}
									start_index = end_index + 1;
								}
								cv::Rect new_rect = cv::Rect();
								for (int i = 0; i < line.size(); ++i) {
									if (temp_counts[i]) {
										new_rect = new_rect | to_rect(line[i]);
									}
								}

								if (new_rect.width == 0) {
									int start_index = 0;
									std::vector<float> temp_ratios(line.size(), 0.0f);
									for (int i = 0; i < line.size(); ++i) {
										const auto& word = line[i];
										int end_index = start_index + word.size();
										int gap = std::min(field_end, end_index) - std::max(field_start, start_index);
										temp_ratios[i] = gap / (float)word.size();
										start_index = end_index + 1;
									}
									int index = distance(temp_ratios.begin(), std::max_element(temp_ratios.begin(), temp_ratios.end()));
									new_rect = to_rect(line[index]);
								}

								std::wstring new_str = line_str.substr(range.start, range.end - range.start);
								ret.push_back(std::make_tuple(new_rect, new_str, cv::Range(0, new_str.length())));
							}
						}
					}
				}
				return ret;
			}
			// UTILS ]]
		};

		class trade_document_recognizer : public recognizer
		{
		public:
			std::pair<std::wstring, int>
				recognize(const std::string& buffer, int languages, const std::string& secret) override
			{
				throw std::logic_error("not implmented");
			}
			std::pair<std::wstring, int>
				recognize(const std::string& buffer, const std::wstring& type, const std::string& secret) override
			{
				throw std::logic_error("not implmented");
			}
			std::unordered_map<std::wstring, std::vector<std::wstring>>
				recognize(const boost::filesystem::path& path, const std::string& secret) override
			{
				const auto configuration = load_configuration(L"trade");
				bool is_batch_mode = std::stoi(configuration.at(L"engine").at(L"batchmode"));
				std::unordered_map<std::wstring, std::vector<std::unordered_map<std::wstring, std::vector<std::wstring>>>> fields;

				std::mutex locks;
				const auto fill_field = [&locks](std::unordered_map<std::wstring, std::vector<std::wstring>>& fields, const std::wstring& field,
					const std::vector<std::wstring>& value) {
					locks.lock();
					if (!value.empty())
						fields.emplace(std::make_pair(field, value));
					locks.unlock();
				};

				std::vector<std::wstring> files;

				if (boost::filesystem::is_directory(path)) {
					for (auto& entry : boost::filesystem::recursive_directory_iterator(path)) {
						const auto file = entry.path();
						const auto extension = boost::algorithm::to_lower_copy(file.extension().native());

						if (extension != L".png" && extension != L".jpg" && extension != L".tif" && extension != L".jp2")
							continue;

						files.emplace_back(boost::filesystem::absolute(file).native());
					}

					std::sort(std::begin(files), std::end(files), compareNat);
				} else {
					const auto extension = boost::algorithm::to_lower_copy(path.extension().native());

					if (extension != L".png" && extension != L".jpg" && extension != L".tif" && extension != L".jp2")
						return std::unordered_map<std::wstring, std::vector<std::wstring>>();

					files.emplace_back(boost::filesystem::absolute(path).native());
				}

				try {
					cv::parallel_for_(cv::Range(0, files.size()), [&](const cv::Range& range) {
						CComPtr<FREngine::IEngineLoader> loader;
						FREngine::IEnginePtr engine;
#if __USE_ENGINE__
						std::tie(loader, engine) = get_engine_object(configuration);
#endif
						FREngine::IClassificationEnginePtr classification_engine;
						FREngine::IModelPtr classification_model;

#if __USE_ENGINE__
						if (!is_batch_mode) {
							classification_engine = engine->CreateClassificationEngine();
							classification_model = classification_engine->
								CreateModelFromFile(get_classification_model(configuration).c_str());
						}
#endif
						for (auto i = range.start; i < range.end; i++) {
							memory_reader memory_reader(boost::filesystem::absolute(files[i]), "");

							if (!is_batch_mode && is_document_filtered(memory_reader.decrypted_)) {
								continue;
							}

							FREngine::IFRDocumentPtr document;
							FREngine::IPagePreprocessingParamsPtr page_preprocessing_params;

							std::wstring category;
							double confidence = 0.;

							auto file_name = boost::filesystem::path(files[i]).filename().native();
#if __USE_ENGINE__
							if (!is_batch_mode) {
								document = engine->CreateFRDocument();
								document->AddImageFileFromStream(&memory_reader, nullptr, nullptr, nullptr,
									boost::filesystem::path(files[i]).filename().native().c_str());
								if (document->Pages->Count < 1) {
									document->Close();
									continue;
								}
								page_preprocessing_params = engine->CreatePagePreprocessingParams();
								page_preprocessing_params->CorrectOrientation = VARIANT_TRUE;
								page_preprocessing_params->OrientationDetectionParams->put_OrientationDetectionMode(FREngine::OrientationDetectionModeEnum::ODM_Thorough);
								document->Preprocess(page_preprocessing_params, nullptr, nullptr, nullptr);
							}
#endif
							std::wstring class_name;

							if (is_batch_mode) {
							class_name = boost::filesystem::path(files[i]).parent_path().filename().native();
							}
							else {
							class_name = classify_document(engine, configuration, classification_engine, classification_model, files[i], document);
							}

							if (class_name.compare(L"BL") == 0) {
								class_name = L"BILL OF LADING";
							}
							else if (class_name.compare(L"INVOICE") == 0) {
								class_name = L"COMMERCIAL INVOICE";
							}
							else if (class_name.compare(L"LC") == 0) {
								class_name = L"LETTER OF CREDIT";
							}
							else if (class_name.compare(L"PROFORMA") == 0) {
								class_name = L"COMMERCIAL INVOICE";
							}
							else if (class_name.compare(L"ETC") == 0) {
								class_name = L"BILL OF EXCHANGE";
							}

							if (!is_batch_mode && class_name.empty()) {
#if __USE_ENGINE__
								document->Close();
#endif
								continue;
							}

							const std::set<std::wstring> processed_class_names{
								L"AIR WAYBILL",
								L"BILL OF EXCHANGE",
								L"BILL OF LADING",
								L"CERTIFICATE",
								L"CERTIFICATE OF ORIGIN",
								L"COMMERCIAL INVOICE",
								L"LETTER OF CREDIT",
								L"PACKING LIST",
								L"INSURANCE POLICY",
								L"SHIPMENT ADVICE",
								L"REMITTANCE LETTER",
								L"LETTER OF GUARANTEE",
								L"FIRM OFFER",
								L"EXPORT PERMIT",
								L"CARGO RECEIPT",
							};

							if (processed_class_names.find(class_name) == processed_class_names.end()) {
#if __USE_ENGINE__
								document->Close();
#endif
								continue;
							}

							std::vector<block> blocks;
							cv::Size image_size;
							std::tie(blocks, std::ignore, image_size) = recognize_document(engine, configuration, class_name, files[i], document, true, true, false, true);

							if (image_size.area() == 0)
								image_size = estimate_paper_size(blocks);
#if __USE_ENGINE__
							if (!is_batch_mode)
								document->Close();
#endif
							if (blocks.empty())
								continue;

							const auto text = to_wstring(blocks);

#ifdef WRITE_TRADE_TXT
							const auto full_output_directory = fmt::format(WRITE_FILE_DIR);
							if (!boost::filesystem::exists(full_output_directory))
								boost::filesystem::create_directories(full_output_directory);
							auto cur = time(NULL);
							auto time = localtime(&cur);
							auto out_file_path = fmt::format(L"{}\\result_{:02d}{:02d}.txt", full_output_directory, time->tm_mon + 1, time->tm_mday);
							auto temp_file_name = boost::filesystem::path(files[i]).filename().native();
							std::wofstream txt_file;
							txt_file.imbue(std::locale(std::locale::empty(), new std::codecvt_utf8<wchar_t, 0x10ffff, std::generate_header>));
							txt_file.open(out_file_path, std::wofstream::out | std::wofstream::app);
							txt_file << temp_file_name << std::endl << std::endl << std::endl;
							txt_file << text << std::endl << std::endl << std::endl;
							txt_file.close();
#endif
#if defined(_DEBUG)
							const auto image = debug_;
#endif
							cv::TickMeter processing_ticks;
							processing_ticks.start();
							const auto keywords = get_keywords(configuration, keywords_, class_name);
							const auto searched_fields = search_fields(class_name, keywords, blocks);

							std::unordered_map<std::wstring, std::vector<std::wstring>> extracted_fields;

							if (class_name == L"BILL OF LADING") {
								const auto place_of_issue = extract_place_of_issue(configuration, class_name, searched_fields, blocks, image_size);
								const auto port_of_loading = extract_port_of_loading(configuration, class_name, searched_fields, blocks, image_size);
								const auto place_of_receipt = extract_place_of_receipt(configuration, class_name, searched_fields, blocks, image_size);
								const auto port_of_discharge = extract_port_of_discharge(configuration, class_name, searched_fields, blocks, image_size);
								const auto place_of_delivery = extract_place_of_delivery(configuration, class_name, searched_fields, blocks, image_size);
								const auto consignee = extract_consignee(configuration, class_name, searched_fields, blocks, image_size);
								const auto shipper = extract_shipper(configuration, class_name, searched_fields, blocks, image_size);
								const auto notify = extract_notify(configuration, class_name, searched_fields, blocks, image_size);
								const auto vessel_name = extract_vessel_name(configuration, class_name, searched_fields, blocks, image_size);
								const auto shipping_line = extract_shipping_line(configuration, class_name, searched_fields, blocks, image_size);
								const auto carrier = extract_carrier(configuration, class_name, searched_fields, blocks, image_size);
								const auto origin = extract_origin(configuration, class_name, searched_fields, blocks, image_size);
								const auto agent = extract_agent(configuration, class_name, searched_fields, blocks, image_size);

								fill_field(extracted_fields, L"PORT OF LOADING", std::vector<std::wstring>{port_of_loading});
								fill_field(extracted_fields, L"PLACE OF RECEIPT", std::vector<std::wstring>{place_of_receipt});
								fill_field(extracted_fields, L"PORT OF DISCHARGE", std::vector<std::wstring>{port_of_discharge});
								fill_field(extracted_fields, L"PLACE OF DELIVERY", std::vector<std::wstring>{place_of_delivery});
								fill_field(extracted_fields, L"CONSIGNEE", consignee);
								fill_field(extracted_fields, L"SHIPEER", shipper);
								fill_field(extracted_fields, L"NOTIFY", notify);
								fill_field(extracted_fields, L"VESSEL NAME", std::vector<std::wstring>{vessel_name});
								fill_field(extracted_fields, L"SHIPPING LINE", std::vector<std::wstring>{shipping_line});
								fill_field(extracted_fields, L"CARRIER", std::vector<std::wstring>{carrier});
								fill_field(extracted_fields, L"PLACE OF ISSUE", std::vector<std::wstring>{place_of_issue});
								fill_field(extracted_fields, L"ORIGIN", std::vector<std::wstring>{origin});
								fill_field(extracted_fields, L"AGENT", agent);
							} else if (class_name == L"CARGO RECEIPT") {
								const auto place_of_issue = extract_place_of_issue(configuration, class_name, searched_fields, blocks, image_size);
								const auto port_of_loading = extract_port_of_loading(configuration, class_name, searched_fields, blocks, image_size);
								const auto place_of_receipt = extract_place_of_receipt(configuration, class_name, searched_fields, blocks, image_size);
								const auto port_of_discharge = extract_port_of_discharge(configuration, class_name, searched_fields, blocks, image_size);
								const auto place_of_delivery = extract_place_of_delivery(configuration, class_name, searched_fields, blocks, image_size);
								const auto consignee = extract_consignee(configuration, class_name, searched_fields, blocks, image_size);
								const auto shipper = extract_shipper(configuration, class_name, searched_fields, blocks, image_size);
								const auto notify = extract_notify(configuration, class_name, searched_fields, blocks, image_size);
								const auto vessel_name = extract_vessel_name(configuration, class_name, searched_fields, blocks, image_size);
								const auto shipping_line = extract_shipping_line(configuration, class_name, searched_fields, blocks, image_size);
								auto agent = extract_agent(configuration, class_name, searched_fields, blocks, image_size);

								fill_field(extracted_fields, L"PORT OF LOADING", std::vector<std::wstring>{port_of_loading});
								fill_field(extracted_fields, L"PLACE OF RECEIPT", std::vector<std::wstring>{place_of_receipt});
								fill_field(extracted_fields, L"PORT OF DISCHARGE", std::vector<std::wstring>{port_of_discharge});
								fill_field(extracted_fields, L"PLACE OF DELIVERY", std::vector<std::wstring>{place_of_delivery});
								fill_field(extracted_fields, L"CONSIGNEE", consignee);
								fill_field(extracted_fields, L"SHIPEER", shipper);
								fill_field(extracted_fields, L"NOTIFY", notify);
								fill_field(extracted_fields, L"VESSEL NAME", std::vector<std::wstring>{vessel_name});
								fill_field(extracted_fields, L"PLACE OF ISSUE", std::vector<std::wstring>{place_of_issue});

								if (!shipping_line.empty()) {
									agent.insert(agent.begin(), shipping_line[0]);
								}
								fill_field(extracted_fields, L"AGENT", agent);
							} else if (class_name == L"LETTER OF CREDIT") {
								const auto goods = extract_goods_description(configuration, class_name, searched_fields, blocks, image_size);
								const auto applicant = extract_applicant(configuration, class_name, searched_fields, blocks, image_size);
								const auto beneficiary = extract_beneficiary(configuration, class_name, searched_fields, blocks, image_size);
								const auto banks = extract_banks(configuration, class_name, searched_fields, blocks, image_size);

								const auto port_of_loading = extract_port_of_loading(configuration, class_name, searched_fields, blocks, image_size);
								const auto port_of_discharge = extract_port_of_discharge(configuration, class_name, searched_fields, blocks, image_size);
								const auto place_of_delivery = extract_place_of_delivery(configuration, class_name, searched_fields, blocks, image_size);

								fill_field(extracted_fields, L"APPLICANT", applicant);
								fill_field(extracted_fields, L"BENEFICIARY", beneficiary);
								fill_field(extracted_fields, L"PORT OF LOADING", std::vector<std::wstring>{port_of_loading});
								fill_field(extracted_fields, L"PORT OF DISCHARGE", std::vector<std::wstring>{port_of_discharge});
								fill_field(extracted_fields, L"PLACE OF DELIVERY", std::vector<std::wstring>{place_of_delivery});
								fill_field(extracted_fields, L"GOODS DESCRIPTION", std::vector<std::wstring>{goods});
								for (const auto& bank : banks) {
									fill_field(extracted_fields, std::get<0>(bank), std::get<1>(bank));
								}
							} else if (class_name == L"COMMERCIAL INVOICE" || class_name == L"PACKING LIST") {
								/*
								const auto goods = extract_goods_description(configuration, class_name, searched_fields, blocks, image_size);

								const auto seller = extract_seller(configuration, class_name, searched_fields, blocks, image_size);
								const auto buyer = extract_buyer(configuration, class_name, searched_fields, blocks, image_size);
								const auto origin = extract_origin(configuration, class_name, searched_fields, blocks, image_size);
								const auto consignee = extract_consignee(configuration, class_name, searched_fields, blocks, image_size);
								const auto notify = extract_notify(configuration, class_name, searched_fields, blocks, image_size);
								const auto port_of_loading = extract_port_of_loading(configuration, class_name, searched_fields, blocks, image_size);
								const auto exporter = extract_exporter_ex(configuration, class_name, searched_fields, blocks, image_size);
								const auto applicant = extract_applicant(configuration, class_name, searched_fields, blocks, image_size);
								const auto manufacturer = extract_manufacturer(configuration, class_name, searched_fields, blocks, image_size);
								const auto importer = extract_importer(configuration, class_name, searched_fields, blocks, image_size);

								const auto vessel_name = extract_vessel_name(configuration, class_name, searched_fields, blocks, image_size);
								const auto port_of_discharge = extract_port_of_discharge(configuration, class_name, searched_fields, blocks, image_size);
								const auto place_of_delivery = extract_place_of_delivery(configuration, class_name, searched_fields, blocks, image_size);
								fill_field(extracted_fields, L"SELLER", seller);
								fill_field(extracted_fields, L"BUYER", buyer);
								fill_field(extracted_fields, L"ORIGIN", std::vector<std::wstring>{origin});
								fill_field(extracted_fields, L"CONSIGNEE", consignee);
								fill_field(extracted_fields, L"NOTIFY", notify);
								fill_field(extracted_fields, L"PORT OF LOADING", std::vector<std::wstring>{port_of_loading});
								fill_field(extracted_fields, L"PORT OF DISCHARGE", std::vector<std::wstring>{port_of_discharge});
								fill_field(extracted_fields, L"PLACE OF DELIVERY", std::vector<std::wstring>{place_of_delivery});
								fill_field(extracted_fields, L"VESSEL NAME", std::vector<std::wstring>{vessel_name});
								fill_field(extracted_fields, L"EXPORTER", std::vector<std::wstring>{exporter} );
								fill_field(extracted_fields, L"APPLICANT", applicant);
								fill_field(extracted_fields, L"MANUFACTURER", manufacturer);
								fill_field(extracted_fields, L"IMPORTER", importer);
								fill_field(extracted_fields, L"GOOD DESCRIPTION", std::vector<std::wstring>{goods});
								*/
								

								
							} else if (class_name == L"BILL OF EXCHANGE") {
								/*const auto banks = extract_banks(configuration, class_name, searched_fields, blocks, image_size);

								for (const auto& bank : banks) {
									fill_field(extracted_fields, std::get<0>(bank), std::get<1>(bank));
								}*/
							} else if (class_name == L"EXPORT PERMIT") {
								const auto goods = extract_goods_description(configuration, class_name, searched_fields, blocks, image_size);
								fill_field(extracted_fields, L"GOODS DESCRIPTION", std::vector<std::wstring>{goods});
							} else if (class_name == L"CERTIFICATE OF ORIGIN") {
								const auto origin = extract_origin(configuration, class_name, searched_fields, blocks, image_size);
								const auto importer = extract_importer(configuration, class_name, searched_fields, blocks, image_size);

								fill_field(extracted_fields, L"ORIGIN", std::vector<std::wstring>{origin});
								fill_field(extracted_fields, L"IMPORTER", importer);
							} else if (class_name == L"INSURANCE POLICY") {
								const auto vessel_name = extract_vessel_name(configuration, class_name, searched_fields, blocks, image_size);
								const auto port_of_loading = extract_port_of_loading(configuration, class_name, searched_fields, blocks, image_size);
								const auto port_of_discharge = extract_port_of_discharge(configuration, class_name, searched_fields, blocks, image_size);
								const auto origin = extract_origin(configuration, class_name, searched_fields, blocks, image_size);
								const auto goods = extract_goods_description(configuration, class_name, searched_fields, blocks, image_size);
								const auto insurance_company = extract_insurance_company(configuration, blocks, image_size);
								const auto insurance_settling_agent = extract_insurance_company(configuration, searched_fields, blocks, image_size, L"INSURANCE SETTLING AGENT");
								const auto insurance_survey_agent = extract_insurance_company(configuration, searched_fields, blocks, image_size, L"INSURANCE SURVEY AGENT");

								fill_field(extracted_fields, L"INSURANCE COMPANY", std::vector<std::wstring>{insurance_company});
								fill_field(extracted_fields, L"INSURANCE SETTLING AGENT", std::vector<std::wstring>{std::get<0>(insurance_settling_agent)});
								fill_field(extracted_fields, L"INSURANCE SETTLING AGENT_CC", std::vector<std::wstring>{std::get<0>(insurance_settling_agent)});
								fill_field(extracted_fields, L"INSURANCE SURVEY AGENT", std::vector<std::wstring>{std::get<0>(insurance_survey_agent)});
								fill_field(extracted_fields, L"INSURANCE SURVEY AGENT_CC", std::vector<std::wstring>{std::get<0>(insurance_survey_agent)});
								fill_field(extracted_fields, L"PORT OF LOADING", std::vector<std::wstring>{port_of_loading});
								fill_field(extracted_fields, L"PORT OF DISCHARGE", std::vector<std::wstring>{port_of_discharge});
								fill_field(extracted_fields, L"VESSEL NAME", std::vector<std::wstring>{vessel_name});
								fill_field(extracted_fields, L"ORIGIN", std::vector<std::wstring>{origin});
								fill_field(extracted_fields, L"GOODS DESCRIPTION", std::vector<std::wstring>{goods});
							} else if (class_name == L"SHIPMENT ADVICE") {
								const auto applicant = extract_applicant(configuration, class_name, searched_fields, blocks, image_size);
								const auto beneficiary = extract_beneficiary(configuration, class_name, searched_fields, blocks, image_size);
								const auto port_of_loading = extract_port_of_loading(configuration, class_name, searched_fields, blocks, image_size);
								const auto port_of_discharge = extract_port_of_discharge(configuration, class_name, searched_fields, blocks, image_size);
								const auto vessel_name = extract_vessel_name(configuration, class_name, searched_fields, blocks, image_size);
								const auto shipping_line = extract_shipping_line(configuration, class_name, searched_fields, blocks, image_size);
								const auto insurance_company = extract_insurance_company(configuration, searched_fields, blocks, image_size, L"INSURANCE COMPANY");
								const auto banks = extract_banks(configuration, class_name, searched_fields, blocks, image_size);

								fill_field(extracted_fields, L"APPLICANT", applicant);
								fill_field(extracted_fields, L"BENEFICIARY", beneficiary);
								fill_field(extracted_fields, L"PORT OF LOADING", std::vector<std::wstring>{port_of_loading});
								fill_field(extracted_fields, L"PORT OF DISCHARGE", std::vector<std::wstring>{port_of_discharge});
								fill_field(extracted_fields, L"VESSEL NAME", std::vector<std::wstring>{vessel_name});
								fill_field(extracted_fields, L"SHIPPING LINE", std::vector<std::wstring>{shipping_line});
								fill_field(extracted_fields, L"INSURANCE COMPANY", std::vector<std::wstring>{std::get<0>(insurance_company)});
								fill_field(extracted_fields, L"INSURANCE COMPANY", std::vector<std::wstring>{std::get<1>(insurance_company)});

								for (const auto& bank : banks) {
									fill_field(extracted_fields, std::get<0>(bank), std::get<1>(bank));
								}
							} else if (class_name == L"CERTIFICATE") {
								const auto goods = extract_goods_description(configuration, class_name, searched_fields, blocks, image_size);
								const auto origin = extract_origin(configuration, class_name, searched_fields, blocks, image_size);
								const auto port_of_loading = extract_port_of_loading(configuration, class_name, searched_fields, blocks, image_size);
								const auto port_of_discharge = extract_port_of_discharge(configuration, class_name, searched_fields, blocks, image_size);
								const auto place_of_delivery = extract_place_of_delivery(configuration, class_name, searched_fields, blocks, image_size);
								const auto vessel_name = extract_vessel_name(configuration, class_name, searched_fields, blocks, image_size);

								fill_field(extracted_fields, L"GOODS DESCRIPTION", std::vector<std::wstring>{goods});
								fill_field(extracted_fields, L"ORIGIN", std::vector<std::wstring>{origin});
								fill_field(extracted_fields, L"PORT OF LOADING", std::vector<std::wstring>{port_of_loading});
								fill_field(extracted_fields, L"PORT OF DISCHARGE", std::vector<std::wstring>{port_of_discharge});
								fill_field(extracted_fields, L"PLACE OF DELIVERY", std::vector<std::wstring>{place_of_delivery});
								fill_field(extracted_fields, L"VESSEL NAME", std::vector<std::wstring>{vessel_name});
							} else if (class_name == L"AIR WAYBILL") {
								const auto port_of_loading = extract_port_of_loading(configuration, class_name, searched_fields, blocks, image_size);
								const auto port_of_discharge = extract_port_of_discharge(configuration, class_name, searched_fields, blocks, image_size);
								const auto consignee = extract_consignee(configuration, class_name, searched_fields, blocks, image_size);
								const auto shipper = extract_shipper(configuration, class_name, searched_fields, blocks, image_size);
								const auto notify = extract_notify(configuration, class_name, searched_fields, blocks, image_size);
								const auto vessel_name = extract_vessel_name(configuration, class_name, searched_fields, blocks, image_size);
								const auto agent = extract_agent(configuration, class_name, searched_fields, blocks, image_size);

								fill_field(extracted_fields, L"PORT OF LOADING", std::vector<std::wstring>{port_of_loading});
								fill_field(extracted_fields, L"PORT OF DISCHARGE", std::vector<std::wstring>{port_of_discharge});
								fill_field(extracted_fields, L"CONSIGNEE", consignee);
								fill_field(extracted_fields, L"SHIPEER", shipper);
								fill_field(extracted_fields, L"NOTIFY", notify);
								fill_field(extracted_fields, L"VESSEL NAME", std::vector<std::wstring>{vessel_name});
								fill_field(extracted_fields, L"AGENT", agent);
							} else if (class_name == L"REMITTANCE LETTER") {
								const auto bank = extract_issuing_bank(configuration, class_name, searched_fields, blocks, image_size);
								const auto drawer = extract_drawer(configuration, class_name, searched_fields, blocks, image_size);
								fill_field(extracted_fields, L"ISSUING BANK", bank);
								fill_field(extracted_fields, L"DRAWER", drawer);
							} else if (class_name == L"FIRM OFFER") {
								const auto goods = extract_goods_description(configuration, class_name, searched_fields, blocks, image_size);
								const auto origin = extract_origin(configuration, class_name, searched_fields, blocks, image_size);
								const auto applicant = extract_applicant(configuration, class_name, searched_fields, blocks, image_size);
								const auto seller = extract_seller(configuration, class_name, searched_fields, blocks, image_size);
								const auto buyer = extract_buyer(configuration, class_name, searched_fields, blocks, image_size);
								const auto banks = extract_banks(configuration, class_name, searched_fields, blocks, image_size);

								fill_field(extracted_fields, L"SELLER", seller);
								fill_field(extracted_fields, L"BUYER", buyer);
								fill_field(extracted_fields, L"ORIGIN", std::vector<std::wstring>{origin});
								fill_field(extracted_fields, L"APPLICANT", applicant);
								for (const auto& bank : banks) {
									fill_field(extracted_fields, std::get<0>(bank), std::get<1>(bank));
								}
								fill_field(extracted_fields, L"GOODS DESCRIPTION", std::vector<std::wstring>{goods});
							} else if (class_name == L"LETTER OF GUARANTEE") {
								const auto port_of_loading = extract_port_of_loading(configuration, class_name, searched_fields, blocks, image_size);
								const auto port_of_discharge = extract_port_of_discharge(configuration, class_name, searched_fields, blocks, image_size);
								const auto consignee = extract_consignee(configuration, class_name, searched_fields, blocks, image_size);
								const auto shipper = extract_shipper(configuration, class_name, searched_fields, blocks, image_size);
								const auto notify = extract_notify(configuration, class_name, searched_fields, blocks, image_size);
								const auto vessel_name = extract_vessel_name(configuration, class_name, searched_fields, blocks, image_size);
								const auto goods = extract_goods_description(configuration, class_name, searched_fields, blocks, image_size);
								const auto shipping_company = extract_shipping_company(configuration, class_name, searched_fields, blocks, image_size);

								fill_field(extracted_fields, L"PORT OF LOADING", std::vector<std::wstring>{port_of_loading});
								fill_field(extracted_fields, L"PORT OF DISCHARGE", std::vector<std::wstring>{port_of_discharge});
								fill_field(extracted_fields, L"CONSIGNEE", consignee);
								fill_field(extracted_fields, L"SHIPEER", shipper);
								fill_field(extracted_fields, L"NOTIFY", notify);
								fill_field(extracted_fields, L"VESSEL NAME", std::vector<std::wstring>{vessel_name});
								fill_field(extracted_fields, L"GOODS DESCRIPTION", std::vector<std::wstring>{goods});
								fill_field(extracted_fields, L"CARRIER", shipping_company);
							}

							fill_field(extracted_fields, L"FILE NAME", std::vector<std::wstring>{file_name});

							processing_ticks.stop();
							spdlog::get("recognizer")->info("process document : {} ({:.2f}mSec)", to_cp949(boost::filesystem::path(files[i]).filename().native()),
								processing_ticks.getTimeMilli());

							if (extracted_fields.empty())
								continue;

							if (fields.find(class_name) == fields.end())
								fields.emplace(class_name, std::vector<std::unordered_map<std::wstring, std::vector<std::wstring>>>());

							fields.at(class_name).emplace_back(extracted_fields);
						}

						release_engine_object(std::make_pair(loader, engine));
					}, std::stoi(configuration.at(L"engine").at(L"concurrency")));
				}
				catch (_com_error& e) {
					spdlog::get("recognizer")->error("exception : {} ({} : {})", to_cp949(e.Description().GetBSTR()), __FILE__, __LINE__);
				}

				std::unordered_map<std::wstring, std::vector<std::wstring>> extracted_fields;

				std::vector<std::wstring> others{ L"OTHER1", L"OTHER2", L"OTHER3", L"OTHER7", L"OTHER8", L"OTHER9", L"OTHER10" };
				const auto fill_others = [&extracted_fields, &others](const std::wstring& field) {
					if (extracted_fields.find(field) != extracted_fields.end()) {
						if (field == L"MANUFACTURER" && extracted_fields.find(L"OTHER4") == extracted_fields.end()) {
							extracted_fields.emplace(std::make_pair(L"OTHER4", extracted_fields.at(field)));
							extracted_fields.erase(field);
							return;
						}

						for (const auto& other : others) {
							if (extracted_fields.find(other) != extracted_fields.end())
								continue;

							if (field == L"AGENT" && extracted_fields.at(field).size() > 1) {
								extracted_fields.emplace(std::make_pair(other, std::vector<std::wstring>{extracted_fields.at(field)[0]}));
								extracted_fields.at(field).erase(extracted_fields.at(field).begin());
							} else {
								extracted_fields.emplace(std::make_pair(other, extracted_fields.at(field)));
								extracted_fields.erase(field);
							}
							break;
						}
					}
				};
				/*
				if (!is_batch_mode) {
					std::vector<std::wstring> categories = {
						L"AIR WAYBILL",              //0
						L"BILL OF EXCHANGE",         //1
						L"BILL OF EXCHANGE-KOR",     //2
						L"BILL OF LADING",           //3
						L"CARGO RECEIPT",            //4
						L"CERTIFICATE",              //5
						L"CERTIFICATE OF ORIGIN",    //6
						L"EXPORT PERMIT",            //7
						L"INSURANCE POLICY",         //8
						L"LETTER OF CREDIT",         //9
						L"PACKING LIST",             //10
						L"REMITTANCE LETTER",        //11
						L"SHIPMENT ADVICE",          //12
						L"WAYBILL",                  //13
						L"COMMERCIAL INVOICE",       //14
						L"LETTER OF GUARANTEE",      //15
						L"FIRM OFFER",               //16
					};

					std::unordered_map<std::wstring, std::vector<std::wstring>> key_values = {
						{ categories[0], { L"PORT OF LOADING", L"PORT OF DISCHARGE", L"CONSIGNEE", L"SHIPPER", L"NOTIFY", L"VESSEL NAME", L"AGENT" } },
						{ categories[1], { L"NEGOTIATION BANK" } },
						{ categories[2], { L"" } },
						{ categories[3], { L"PORT OF LOADING", L"PLACE OF RECEIPT", L"PORT OF DISCHARGE", L"PLACE OF DELIVERY", L"CONSIGNEE", L"SHIPPER", L"NOTIFY", L"VESSEL NAME", L"CARRIER", L"PLACE OF ISSUE", L"ORIGIN", L"SHIPPING LINE" } },
						{ categories[4], { L"PORT OF LOADING", L"PLACE OF RECEIPT", L"PORT OF DISCHARGE", L"PLACE OF DELIVERY", L"CONSIGNEE", L"SHIPPER", L"NOTIFY", L"VESSEL NAME", L"PLACE OF ISSUE" } },
						{ categories[5], { L"ORIGIN", L"GOODS DESCRIPTION", L"PORT OF LOADING", L"PLACE OF DELIVERY", L"PORT OF DISCHARGE", L"VESSEL NAME", } },
						{ categories[6], { L"ORIGIN", L"IMPORTER" } },
						{ categories[7], { L"GOODS DESCRIPTION" } },
						{ categories[8], { L"INSURANCE COMPANY", L"INSURANCE SETTLING AGENT", L"INSURANCE SURVEY AGENT", L"PORT OF LOADING", L"PORT OF DISCHARGE", L"VESSEL NAME", L"ORIGIN", L"GOODS DESCRIPTION" } },
						{ categories[9], { L"ISSUING BANK", L"COLLECTING BANK", L"APPLICANT", L"BENEFICIARY", L"PORT OF LOADING", L"PORT OF DISCHARGE" } },
						{ categories[10], { L"SELLER", L"BUYER", L"ORIGIN", L"CONSIGNEE", L"NOTIFY", L"PORT OF LOADING", L"EXPORTER", L"APPLICANT", L"MANUFACTURER", L"PORT OF DISCHARGE", L"PLACE OF DELIVERTY", L"VESSEL NAME" } },
						{ categories[11], { L"ISSUING BANK", L"DRAWER" } },
						{ categories[12], { L"APPLICANT", L"BENEFICIARY", L"PORT OF LOADING", L"PORT OF DISCHARGE", L"VESSEL NAME", L"SHIPPING LINE", L"INSURANCE COMPANY" } },
						{ categories[13], { L"", } },
						//{ categories[14], { L"SELLER", L"BUYER", L"ORIGIN", L"CONSIGNEE", L"NOTIFY", L"PORT OF LOADING", L"EXPORTER", L"APPLICANT", L"MANUFACTURER", L"PORT OF DISCHARGE", L"PLACE OF DELIVERY", L"VESSEL NAME" } },
						{ categories[14],{ L"SELLER", L"BUYER", L"ORIGIN", L"CONSIGNEE", L"NOTIFY", L"PORT OF LOADING", L"EXPORTER", L"APPLICANT", L"MANUFACTURER", L"PORT OF DISCHARGE", L"PLACE OF DELIVERY", L"VESSEL NAME", L"L/C NO" } },
						{ categories[15], { L"CARRIER", L"SHOPPER", L"PORT OF LOADING", L"PORT OF DISCHARGE", L"VESSEL NAME", L"CONSIGNEE", L"GOODS DESCRIPTION", L"NOTIFY" } },
						{ categories[16], { L"SELLER", L"BUYER", L"ORIGIN", L"APPLICANT", L"COLLECTING BANK" } },
					};
					
					auto cur = time(NULL);
					auto time = localtime(&cur);
					auto output_path = fmt::format(L"{}\\result_{:02d}{:02d}.txt", path.parent_path().native(), time->tm_mon + 1, time->tm_mday);
					auto set_name = path.filename().native();
					std::wstring result_text;

					for (auto& category : categories) {
						if (fields.find(category) == fields.end()) {
							continue;
						}
						auto& category_results = fields.at(category);

						auto& keys = key_values.at(category);
						int count = 1;
						for (auto& result : category_results) {
							result_text += fmt::format(L"\"set name\",\"{}\"\n", set_name);
							result_text += fmt::format(L"\"file name\",\"{}\"\n", result.at(L"FILE NAME").front());
							result_text += fmt::format(L"\"category\",\"{}\"\n", category);
							result_text += fmt::format(L"\"category number\",\"{}\"\n", count);
							count++;
							for (auto& key : keys) {
								auto& name = key;
								result_text += L"\"" + name + L"\",";
								if (result.find(name) == result.end()) {
									result_text += L"\n";
									continue;
								}
								auto& r = result.at(name);

								for (auto& f : r) {
									result_text += L"\"" + f + L"\",";
								}
								result_text += L"\n";
							}
							result_text += L"\n";
						}
					}
					std::wofstream txt_file;
					txt_file.imbue(std::locale(std::locale::locale::empty(), new std::codecvt_utf8<wchar_t, 0x10ffff, std::generate_header>));
					txt_file.open(output_path, std::wofstream::out | std::wofstream::app);
					txt_file << result_text << std::endl;
					txt_file.close();
					
				}
*/
				// TODO:
				// 랭킹 알고리즘 개발해서 필드값 전달
				// 그냥 단순하게 제이 처음 나온 필드를 선택하도록 구현됨

				const std::set<std::wstring> category_order{
					L"EXPORT PERMIT",
					L"COMMERCIAL INVOICE",
					L"PACKING LIST",
					L"BILL OF LADING",
					L"AIR WAYBILL",
					L"CARGO RECEIPT",
					L"INSURANCE POLICY",
					L"FIRM OFFER",
					L"BILL OF EXCHANGE",
					L"CERTIFICATE",
					L"CERTIFICATE OF ORIGIN",
					L"SHIPMENT ADVICE",
					L"REMITTANCE LETTER",
					L"LETTER OF CREDIT",
					L"LETTER OF GUARANTEE",
				};

				for (const auto& category : category_order) {
					if (fields.find(category) != fields.end()) {
						for (const auto& document : fields.at(category)) {
							for (const auto& field : document) {
								const auto field_name = std::get<0>(field);
								const auto field_values = std::get<1>(field);
								if (field_name == L"ISSUING BANK" || field_name == L"COLLECTING BANK" || field_name == L"CONSIGNEE" || field_name == L"NEGOTIATION BANK") {
									bool include_scbank = false;
									for (auto field_value : field_values) {
										if (boost::regex_search(field_value, boost::wregex(L"STANDARD CHARTERED", boost::regex::icase))
											|| boost::regex_search(field_value, boost::wregex(L"STANDARD CHART", boost::regex::icase))
											|| boost::regex_search(field_value, boost::wregex(L"스(?:탠|턴)다드"))
											|| boost::regex_search(field_value, boost::wregex(L"차타드"))) {
											include_scbank = true;
											break;
										}
									}
									if (include_scbank)
										continue;
								}

								if (field_values.size() == 1 && field_values[0].empty())
									continue;

								if (boost::regex_search(field_values[0], boost::wregex(L"SAME (?:AS)? (?:ABOVE|BELOW|CONSIGNEE)")))
									continue;

								if (extracted_fields.find(field_name) == extracted_fields.end())
									extracted_fields.emplace(std::make_pair(field_name, field_values));
								else {
									if (extracted_fields.at(field_name).size() < 3
										&& extracted_fields.at(field_name).size() < field_values.size()
										&& field_name != L"ISSUING BANK") {
										extracted_fields.erase(field_name);
										extracted_fields.emplace(std::make_pair(field_name, field_values));
									}
								}
							}
						}
					}
				}

				if (!is_batch_mode) {
					if (extracted_fields.find(L"BENEFICIARY") == extracted_fields.end()) {
						if (extracted_fields.find(L"SELLER") != extracted_fields.end()) {
							extracted_fields.emplace(std::make_pair(L"BENEFICIARY", extracted_fields.at(L"SELLER")));
							extracted_fields.erase(L"SELLER");
						} else if (extracted_fields.find(L"EXPORTER") != extracted_fields.end()) {
							extracted_fields.emplace(std::make_pair(L"BENEFICIARY", extracted_fields.at(L"EXPORTER")));
							extracted_fields.erase(L"EXPORTER");
						} else if (extracted_fields.find(L"SHIPPER") != extracted_fields.end()) {
							extracted_fields.emplace(std::make_pair(L"BENEFICIARY", extracted_fields.at(L"SHIPPER")));
							extracted_fields.erase(L"SHIPPER");
						} else if (extracted_fields.find(L"DRAWER") != extracted_fields.end()) {
							extracted_fields.emplace(std::make_pair(L"BENEFICIARY", extracted_fields.at(L"DRAWER")));
							extracted_fields.erase(L"DRAWER");
						}
					}

					if (extracted_fields.find(L"APPLICANT") == extracted_fields.end()) {
						if (extracted_fields.find(L"BUYER") != extracted_fields.end()) {
							extracted_fields.emplace(std::make_pair(L"APPLICANT", extracted_fields.at(L"BUYER")));
							extracted_fields.erase(L"BUYER");
						} else if (extracted_fields.find(L"DRAWEE") != extracted_fields.end()) {
							extracted_fields.emplace(std::make_pair(L"APPLICANT", extracted_fields.at(L"DRAWEE")));
							extracted_fields.erase(L"DRAWEE");
						} else if (extracted_fields.find(L"IMPORTER") != extracted_fields.end()) {
							extracted_fields.emplace(std::make_pair(L"APPLICANT", extracted_fields.at(L"IMPORTER")));
							extracted_fields.erase(L"IMPORTER");
						}
					}

					if (extracted_fields.find(L"SHIPPER") != extracted_fields.end() &&
						extracted_fields.find(L"BENEFICIARY") != extracted_fields.end()) {
						auto shipper = extracted_fields.at(L"SHIPPER")[0];
						boost::remove_erase_if(shipper, boost::is_any_of(L" .,"));
						auto beneficiary = extracted_fields.at(L"BENEFICIARY")[0];
						boost::remove_erase_if(beneficiary, boost::is_any_of(L" .,"));

						if (SymSpell::DamerauLevenshteinDistance(shipper, beneficiary) < 2) {
							if (extracted_fields.at(L"BENEFICIARY").size() < extracted_fields.at(L"SHIPPER").size()) {
								extracted_fields.erase(L"BENEFICIARY");
								extracted_fields.emplace(std::make_pair(L"BENEFICIARY", extracted_fields.at(L"SHIPPER")));
							}
							extracted_fields.erase(L"SHIPPER");
						}
					}

					if (extracted_fields.find(L"NOTIFY") != extracted_fields.end() &&
						extracted_fields.find(L"APPLICANT") != extracted_fields.end()) {
						auto notify = extracted_fields.at(L"NOTIFY")[0];
						boost::remove_erase_if(notify, boost::is_any_of(L" .,"));
						auto applicant = extracted_fields.at(L"APPLICANT")[0];
						boost::remove_erase_if(applicant, boost::is_any_of(L" .,"));

						if (SymSpell::DamerauLevenshteinDistance(notify, applicant) < 2) {
							extracted_fields.erase(L"NOTIFY");
						}
					}

					if (extracted_fields.find(L"SHIPPING LINE") != extracted_fields.end()) {
						if (extracted_fields.find(L"CARRIER") == extracted_fields.end() ||
							(extracted_fields.find(L"CARRIER") != extracted_fields.end() &&
							extracted_fields.at(L"CARRIER")[0] == extracted_fields.at(L"SHIPPING LINE")[0])) {
							extracted_fields.emplace(std::make_pair(L"CARRIER", extracted_fields.at(L"SHIPPING LINE")));
							extracted_fields.erase(L"SHIPPING LINE");
						}
					}

					std::vector<std::wstring> insurance_company_fields{ L"INSURANCE COMPANY", L"INSURANCE SETTLING AGENT", L"INSURANCE SURVEY AGENT" };
					std::vector<bool> insurance_companies(insurance_company_fields.size(), true);
					for (auto i = 0; i < insurance_company_fields.size(); i++) {
						for (auto j = i + 1; j < insurance_company_fields.size(); j++) {
							if (!insurance_companies[j] ||
								extracted_fields.find(insurance_company_fields[i]) == extracted_fields.end() ||
								extracted_fields.find(insurance_company_fields[j]) == extracted_fields.end())
								continue;

							const auto cleaned_i = boost::regex_replace(extracted_fields.at(insurance_company_fields[i])[0], boost::wregex(L"[^a-zA-Z0-9&-]"), L"");
							const auto cleaned_j = boost::regex_replace(extracted_fields.at(insurance_company_fields[j])[0], boost::wregex(L"[^a-zA-Z0-9&-]"), L"");
							const auto threshold = std::max(1, std::min(3, static_cast<int>(std::min(cleaned_i.size(), cleaned_j.size()) / 2.5)));
							if (SymSpell::DamerauLevenshteinDistance(cleaned_i, cleaned_j) < threshold)
								insurance_companies[j] = false;
						}
					}

					for (auto i = 1; i < insurance_company_fields.size(); i++) {
						if (extracted_fields.find(insurance_company_fields[i]) == extracted_fields.end() || insurance_companies[i])
							continue;

						extracted_fields.erase(insurance_company_fields[i]);
					}

					fill_others(L"INSURANCE SETTLING AGENT");
					fill_others(L"INSURANCE SURVEY AGENT");
					fill_others(L"MANUFACTURER");

					std::unordered_map<std::wstring, std::vector<std::wstring>> extracted_cc_fields;
					const auto fill_country_code = [&extracted_fields, &extracted_cc_fields, &configuration](const std::wstring& field) {
						if (extracted_fields.find(field) != extracted_fields.end()) {
							for (auto i = extracted_fields.at(field).size() - 1; i > 0; i--) {
								const auto last_line = extracted_fields.at(field)[i];
								const auto country = preprocess_country(std::make_pair(cv::Rect(), last_line), configuration, L"countries");
								const auto country_str = boost::algorithm::trim_copy(to_wstring(country));

								if (!country_str.empty()) {
									const auto country_code = create_postprocess_function(configuration, L"country-countrycode.csv")(country_str);
									if (!country_code.empty()) {
										extracted_cc_fields.emplace(std::make_pair(fmt::format(L"{}_CC", field), std::vector<std::wstring>{country_code}));
										break;
									}
								}
							}

						}
					};

					std::set<std::wstring> country_code_ignore_fields{ L"VESSEL NAME", L"ORIGIN", L"SHIPPING LINE", L"GOODS DESCRIPTION", L" PORT OF LOADING", L"PORT OF DISCHARGE", L"PORT OF DELIVERY", L"AGENT", L"CARRIER" };

					for (const auto& field : extracted_fields) {
						const auto field_name = std::get<0>(field);

						if (country_code_ignore_fields.find(field_name) == country_code_ignore_fields.end())
							fill_country_code(field_name);
					}

					if (extracted_fields.find(L"PLACE OF ISSUE") != extracted_fields.end()) {
						std::set<std::wstring> country_code_bl_fields{
							L"CARRIER", L"AGENT", L"SHIPPING LINE"
						};

						const auto country_code = create_postprocess_function(configuration, L"country-countrycode.csv")(extracted_fields.at(L"PLACE OF ISSUE")[0]);

						for (const auto& field : extracted_fields) {
							const auto field_name = std::get<0>(field);
							const auto field_cc_name = field_name + L"_CC";

							if (country_code_bl_fields.find(field_name) != country_code_bl_fields.end()) {
								if (extracted_cc_fields.find(field_cc_name) == extracted_cc_fields.end()) {

									std::vector<std::wstring> field_ccs;

									for (auto i = 0; i < extracted_fields.at(field_name).size(); i++) {
										field_ccs.emplace_back(country_code);
									}

									extracted_cc_fields.emplace(std::make_pair(field_cc_name, field_ccs));
								}
							}
						}
					}

					if (extracted_fields.find(L"AGENT") != extracted_fields.end()
						&& extracted_fields.at(L"AGENT").size() > 1)
						fill_others(L"AGENT");

					std::set<std::wstring> country_code_bank_fields{
						L"ISSUING BANK", L"COLLECTING BANK",
					};

					for (const auto& field : extracted_fields) {
						const auto field_name = std::get<0>(field);
						const auto field_cc_name = field_name + L"_BIC_CC";

						if (country_code_bank_fields.find(field_name) != country_code_bank_fields.end() && extracted_fields.find(field_cc_name) == extracted_fields.end()) {
							if (extracted_fields.at(field_name)[0].size() > 5) {
								const auto swift_code = boost::to_lower_copy(extracted_fields.at(field_name)[0].substr(4, 2));
								const auto country_code = create_postprocess_function(configuration, L"alpha2-countrycode.csv")(swift_code);
								extracted_cc_fields.emplace(std::make_pair(field_cc_name, std::vector<std::wstring>{country_code}));
							}
						}
					}


					extracted_fields.insert(std::begin(extracted_cc_fields), std::end(extracted_cc_fields));
				}

				return extracted_fields;

			}

			std::unordered_map<std::wstring, std::vector<std::wstring>>
				recognize(const boost::filesystem::path& path,
				const std::wstring& class_name) override
			{
				const auto configuration = load_configuration(L"trade");
				//bool is_batch_mode = false;// std::stoi(configuration.at(L"engine").at(L"batchmode"));
				bool is_batch_mode = std::stoi(configuration.at(L"engine").at(L"batchmode"));

				std::unordered_map<std::wstring, std::vector<std::unordered_map<std::wstring, std::vector<std::wstring>>>> fields;

				std::mutex locks;
				const auto fill_field = [&locks](std::unordered_map<std::wstring, std::vector<std::wstring>>& fields, const std::wstring& field,
					const std::vector<std::wstring>& value) {
					locks.lock();
					if (!value.empty())
						fields.emplace(std::make_pair(field, value));
					locks.unlock();
				};

				std::vector<std::wstring> files;

				if (boost::filesystem::is_directory(path)) {
					for (auto& entry : boost::filesystem::recursive_directory_iterator(path)) {
						const auto file = entry.path();
						const auto extension = boost::algorithm::to_lower_copy(file.extension().native());

						if (extension != L".png" && extension != L".jpg" && extension != L".tif" && extension != L".jp2")
							continue;

						files.emplace_back(boost::filesystem::absolute(file).native());
					}

					std::sort(std::begin(files), std::end(files), compareNat);
				} else {
					const auto extension = boost::algorithm::to_lower_copy(path.extension().native());

					if (extension != L".png" && extension != L".jpg" && extension != L".tif" && extension != L".jp2")
						return std::unordered_map<std::wstring, std::vector<std::wstring>>();

					files.emplace_back(boost::filesystem::absolute(path).native());
				}

				try {
					//cv::parallel_for_(cv::Range(0, files.size()), [&](const cv::Range& range) {
						CComPtr<FREngine::IEngineLoader> loader;
						FREngine::IEnginePtr engine;
#if __USE_ENGINE__
						std::tie(loader, engine) = get_engine_object(configuration);
#endif
						//FREngine::IClassificationEnginePtr classification_engine;
						//FREngine::IModelPtr classification_model;

						//if (!is_batch_mode) {
						//	classification_engine = engine->CreateClassificationEngine();
						//	classification_model = classification_engine->
						//		CreateModelFromFile(get_classification_model(configuration).c_str());
						//}

						for (auto i = 0; i < files.size(); i++) {
							memory_reader memory_reader(boost::filesystem::absolute(files[i]), "");

							if (!is_batch_mode && is_document_filtered(memory_reader.decrypted_)) {
								continue;
							}


							const bool need_serialization = needSerialization(configuration, class_name, files[i]);

							FREngine::IFRDocumentPtr document;
							FREngine::IPagePreprocessingParamsPtr page_preprocessing_params;

							std::wstring category;
							double confidence = 0.;

							auto file_name = boost::filesystem::path(files[i]).filename().native();
#if __USE_ENGINE__
							if (!is_batch_mode && need_serialization) {
								document = engine->CreateFRDocument();
								document->AddImageFileFromStream(&memory_reader, nullptr, nullptr, nullptr,
									boost::filesystem::path(files[i]).filename().native().c_str());
								if (document->Pages->Count < 1) {
									document->Close();
									continue;
								}
								page_preprocessing_params = engine->CreatePagePreprocessingParams();
								page_preprocessing_params->CorrectOrientation = VARIANT_TRUE;
								page_preprocessing_params->OrientationDetectionParams->put_OrientationDetectionMode(FREngine::OrientationDetectionModeEnum::ODM_Thorough);
								document->Preprocess(page_preprocessing_params, nullptr, nullptr, nullptr);
							}

#endif
							/*if (is_batch_mode) {
								class_name = boost::filesystem::path(files[i]).parent_path().filename().native();
								}
								else {
								class_name = classify_document(engine, configuration, classification_engine, classification_model, files[i], document);
								}*/

							if (!is_batch_mode && class_name.empty()) {
								if (need_serialization) {
#if __USE_ENGINE__
									document->Close();
#endif
								}
								continue;
							}

							const std::set<std::wstring> processed_class_names{
								//L"AIR WAYBILL",
								//L"BILL OF EXCHANGE",
								L"BILL OF LADING",
								//L"CERTIFICATE",
								//L"CERTIFICATE OF ORIGIN",
								L"COMMERCIAL INVOICE",
								L"LETTER OF CREDIT",
								//L"PACKING LIST",
								//L"INSURANCE POLICY",
								//L"SHIPMENT ADVICE",
								//L"REMITTANCE LETTER",
								//L"LETTER OF GUARANTEE",
								//L"FIRM OFFER",
								//L"EXPORT PERMIT",
								//L"CARGO RECEIPT",
							};

							if (processed_class_names.find(class_name) == processed_class_names.end()) {
								if (need_serialization) {
#if __USE_ENGINE__
									document->Close();
#endif
								}
								continue;
							}

							std::vector<block> blocks;
							cv::Size image_size;
							std::tie(blocks, std::ignore, image_size) = recognize_document(engine, configuration, class_name, files[i], document, true, true, false, true);

							if (image_size.area() == 0)
								image_size = estimate_paper_size(blocks);

							if (!is_batch_mode) {
								if (need_serialization) {
#if __USE_ENGINE__
									document->Close();
#endif
								}
							}

							if (blocks.empty())
								continue;

							const auto text = to_wstring(blocks);

#ifdef WRITE_TRADE_TXT
							const auto full_output_directory = fmt::format(WRITE_FILE_DIR);
							if (!boost::filesystem::exists(full_output_directory))
								boost::filesystem::create_directories(full_output_directory);
							auto cur = time(NULL);
							auto time = localtime(&cur);
							auto out_file_path = fmt::format(L"{}\\result_{:02d}{:02d}.txt", full_output_directory, time->tm_mon + 1, time->tm_mday);
							auto temp_file_name = boost::filesystem::path(files[i]).filename().native();
							std::wofstream txt_file;
							txt_file.imbue(std::locale(std::locale::empty(), new std::codecvt_utf8<wchar_t, 0x10ffff, std::generate_header>));
							txt_file.open(out_file_path, std::wofstream::out | std::wofstream::app);
							txt_file << temp_file_name << std::endl << std::endl << std::endl;
							txt_file << text << std::endl << std::endl << std::endl;
							txt_file.close();
#endif
#if defined(_DEBUG)
							const auto image = debug_;
#endif
							cv::TickMeter processing_ticks;
							processing_ticks.start();
							const auto keywords = get_keywords(configuration, keywords_, class_name);
							const auto searched_fields = search_fields(class_name, keywords, blocks);

							std::unordered_map<std::wstring, std::vector<std::wstring>> extracted_fields;

							if (class_name == L"BILL OF LADING") {
								const auto consignee = extract_consignee(configuration, class_name, searched_fields, blocks, image_size);
								const auto carrier = extract_carrier(configuration, class_name, searched_fields, blocks, image_size);
								const auto notify = extract_notify(configuration, class_name, searched_fields, blocks, image_size);
								const auto port_of_discharge = extract_port_of_discharge(configuration, class_name, searched_fields, blocks, image_size);
								const auto port_of_loading = extract_port_of_loading(configuration, class_name, searched_fields, blocks, image_size);
								const auto shipment_date = extract_shipment_date(configuration, class_name, searched_fields, blocks, image_size);
								const auto vessel_name = extract_vessel_name(configuration, class_name, searched_fields, blocks, image_size);

								fill_field(extracted_fields, L"CONSIGNEE", consignee);
								fill_field(extracted_fields, L"CARRIER", std::vector<std::wstring>{carrier});
								fill_field(extracted_fields, L"NOTIFY", notify);
								fill_field(extracted_fields, L"PORT OF DISCHARGE", port_of_discharge);
								fill_field(extracted_fields, L"PORT OF LOADING", port_of_loading);
								fill_field(extracted_fields, L"SHIPMENT DATE", shipment_date);
								fill_field(extracted_fields, L"VESSEL NAME", std::vector<std::wstring>{vessel_name});
								
							} else if (class_name == L"LETTER OF CREDIT") {
								const auto swift_mt_sender = extract_swift_mt_sender(configuration, class_name, searched_fields, blocks, image_size);
								const auto credit_number = extract_credit_number(configuration, class_name, searched_fields, blocks, image_size);
								const auto amount = extract_amount(configuration, class_name, searched_fields, blocks, image_size);
								fill_field(extracted_fields, L"SWIFT MT SENDER", swift_mt_sender);
								fill_field(extracted_fields, L"CREDIT NUMBER", credit_number);
								fill_field(extracted_fields, L"AMOUNT", std::vector<std::wstring>{amount});
								
							} else if (class_name == L"COMMERCIAL INVOICE") {
								
								const auto seller = extract_seller(configuration, class_name, searched_fields, blocks, image_size);
								//const auto lc_no = extract_lcno(configuration, class_name, searched_fields, blocks, image_size);
								//const auto amount = extract_amount(configuration, class_name, searched_fields, blocks, image_size);
								const auto lc_number = extract_lc_number(configuration, class_name, searched_fields, blocks, image_size);
								const auto currency_sign = extract_currency_sign(configuration, class_name, searched_fields, blocks, image_size);
								const auto amount = extract_amount(configuration, class_name, searched_fields, blocks, image_size);
								std::wstring currency_amount = currency_sign + amount ;
								fill_field(extracted_fields, L"SELLER", seller);								
								//fill_field(extracted_fields, L"L/C NO", lc_no);
								//fill_field(extracted_fields, L"AMOUNT", amount);
								fill_field(extracted_fields, L"L/C NO", std::vector<std::wstring>{lc_number});
								//fill_field(extracted_fields, L"CURRENCY SIGN", std::vector<std::wstring>{currency_sign});
								fill_field(extracted_fields, L"AMOUNT", std::vector<std::wstring>{currency_amount});
								//fill_field(extracted_fields, L"L/C NO", std::vector<std::wstring>{lc_no});
								//fill_field(extracted_fields, L"GOOD DESCRIPTION", std::vector<std::wstring>{goods});
							} 

							fill_field(extracted_fields, L"FILE NAME", std::vector<std::wstring>{file_name});

							processing_ticks.stop();
							spdlog::get("recognizer")->info("process document : {} ({:.2f}mSec)", to_cp949(boost::filesystem::path(files[i]).filename().native()),
								processing_ticks.getTimeMilli());

							if (extracted_fields.empty())
								continue;

							if (fields.find(class_name) == fields.end())
								fields.emplace(class_name, std::vector<std::unordered_map<std::wstring, std::vector<std::wstring>>>());

							fields.at(class_name).emplace_back(extracted_fields);
						}

						release_engine_object(std::make_pair(loader, engine));
						//}, std::stoi(configuration.at(L"engine").at(L"concurrency")));

				}
				catch (_com_error& e) {
					spdlog::get("recognizer")->error("exception : {} ({} : {})", to_cp949(e.Description().GetBSTR()), __FILE__, __LINE__);
				}

				if (!is_batch_mode) {
					std::vector<std::wstring> categories = {
						L"AIR WAYBILL",              //0
						L"BILL OF EXCHANGE",         //1
						L"BILL OF EXCHANGE-KOR",     //2
						L"BILL OF LADING",           //3
						L"CARGO RECEIPT",            //4
						L"CERTIFICATE",              //5
						L"CERTIFICATE OF ORIGIN",    //6
						L"EXPORT PERMIT",            //7
						L"INSURANCE POLICY",         //8
						L"LETTER OF CREDIT",         //9
						L"PACKING LIST",             //10
						L"REMITTANCE LETTER",        //11
						L"SHIPMENT ADVICE",          //12
						L"WAYBILL",                  //13
						L"COMMERCIAL INVOICE",       //14
						L"LETTER OF GUARANTEE",      //15
						L"FIRM OFFER",               //16
					};

					std::unordered_map<std::wstring, std::vector<std::wstring>> key_values = {
						{ categories[0],{ L"PORT OF LOADING", L"PORT OF DISCHARGE", L"CONSIGNEE", L"SHIPPER", L"NOTIFY", L"VESSEL NAME", L"AGENT" } },
						{ categories[1],{ L"NEGOTIATION BANK" } },
						{ categories[2],{ L"" } },
						{ categories[3],{ L"CONSIGNEE", L"CARRIER", L"NOTIFY", L"PORT OF DISCHARGE", L"PORT OF LOADING", L"SHIPMENT DATE", L"VESSEL NAME"} },
						//{ categories[3],{ L"PORT OF LOADING", L"PLACE OF RECEIPT", L"PORT OF DISCHARGE", L"PLACE OF DELIVERY", L"CONSIGNEE", L"SHIPPER", L"NOTIFY", L"VESSEL NAME", L"CARRIER", L"PLACE OF ISSUE", L"ORIGIN", L"SHIPPING LINE" } },
						{ categories[4],{ L"PORT OF LOADING", L"PLACE OF RECEIPT", L"PORT OF DISCHARGE", L"PLACE OF DELIVERY", L"CONSIGNEE", L"SHIPPER", L"NOTIFY", L"VESSEL NAME", L"PLACE OF ISSUE" } },
						{ categories[5],{ L"ORIGIN", L"GOODS DESCRIPTION", L"PORT OF LOADING", L"PLACE OF DELIVERY", L"PORT OF DISCHARGE", L"VESSEL NAME", } },
						{ categories[6],{ L"ORIGIN", L"IMPORTER" } },
						{ categories[7],{ L"GOODS DESCRIPTION" } },
						{ categories[8],{ L"INSURANCE COMPANY", L"INSURANCE SETTLING AGENT", L"INSURANCE SURVEY AGENT", L"PORT OF LOADING", L"PORT OF DISCHARGE", L"VESSEL NAME", L"ORIGIN", L"GOODS DESCRIPTION" } },
						//{ categories[9],{ L"ISSUING BANK", L"COLLECTING BANK", L"APPLICANT", L"BENEFICIARY", L"PORT OF LOADING", L"PORT OF DISCHARGE" } },
						{ categories[9],{ L"SWIFT MT SENDER", L"AMOUNT", L"CREDIT NUMBER" } },
						{ categories[10],{ L"SELLER", L"BUYER", L"ORIGIN", L"CONSIGNEE", L"NOTIFY", L"PORT OF LOADING", L"EXPORTER", L"APPLICANT", L"MANUFACTURER", L"PORT OF DISCHARGE", L"PLACE OF DELIVERTY", L"VESSEL NAME" } },
						{ categories[11],{ L"ISSUING BANK", L"DRAWER" } },
						{ categories[12],{ L"APPLICANT", L"BENEFICIARY", L"PORT OF LOADING", L"PORT OF DISCHARGE", L"VESSEL NAME", L"SHIPPING LINE", L"INSURANCE COMPANY" } },
						{ categories[13],{ L"", } },
						//{ categories[14], { L"SELLER", L"BUYER", L"ORIGIN", L"CONSIGNEE", L"NOTIFY", L"PORT OF LOADING", L"EXPORTER", L"APPLICANT", L"MANUFACTURER", L"PORT OF DISCHARGE", L"PLACE OF DELIVERY", L"VESSEL NAME" } },
						{ categories[14],{ L"SELLER", L"L/C NO", L"AMOUNT" } },
						//{ categories[14],{ L"SELLER", L"L/C NO", L"CURRENCY SIGN", L"AMOUNT" } },
						{ categories[15],{ L"CARRIER", L"SHOPPER", L"PORT OF LOADING", L"PORT OF DISCHARGE", L"VESSEL NAME", L"CONSIGNEE", L"GOODS DESCRIPTION", L"NOTIFY" } },
						{ categories[16],{ L"SELLER", L"BUYER", L"ORIGIN", L"APPLICANT", L"COLLECTING BANK" } },
					};

					auto cur = time(NULL);
					auto time = localtime(&cur);
					auto output_path = fmt::format(L"{}\\result_{:02d}{:02d}.txt", path.parent_path().native(), time->tm_mon + 1, time->tm_mday);
					auto set_name = path.filename().native();
					std::wstring result_text;

					for (auto& category : categories) {
						if (fields.find(category) == fields.end()) {
							continue;
						}
						auto& category_results = fields.at(category);

						auto& keys = key_values.at(category);
						int count = 1;
						for (auto& result : category_results) {
							result_text += fmt::format(L"\"set name\",\"{}\"\n", set_name);
							result_text += fmt::format(L"\"file name\",\"{}\"\n", result.at(L"FILE NAME").front());
							result_text += fmt::format(L"\"category\",\"{}\"\n", category);
							result_text += fmt::format(L"\"category number\",\"{}\"\n", count);
							count++;
							for (auto& key : keys) {
								auto& name = key;
								result_text += L"\"" + name + L"\",";
								if (result.find(name) == result.end()) {
									result_text += L"\n";
									continue;
								}
								auto& r = result.at(name);

								for (auto& f : r) {
									result_text += L"\"" + f + L"\",";
								}
								result_text += L"\n";
							}
							result_text += L"\n";
						}
					}
					std::wofstream txt_file;
					txt_file.imbue(std::locale(std::locale::locale::empty(), new std::codecvt_utf8<wchar_t, 0x10ffff, std::generate_header>));
					txt_file.open(output_path, std::wofstream::out | std::wofstream::app);
					txt_file << result_text << std::endl;
					txt_file.close();

				}

				std::unordered_map<std::wstring, std::vector<std::wstring>> extracted_fields;

				if (fields.find(class_name) != fields.end()) {
					extracted_fields = fields.at(class_name).front();
				}
				return extracted_fields;
			}

		private:
			static std::wstring
				extract_port_name(const configuration& configuration, const std::wstring& category, const std::wstring& field_name,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
				const std::vector<block>& blocks)
			{
				if (fields.find(field_name) == fields.end())
					return L"";

				std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>> filtered_fields;
				std::copy_if(std::begin(fields.at(field_name)), std::end(fields.at(field_name)), std::back_inserter(filtered_fields), [](const std::tuple<cv::Rect, std::wstring, cv::Range>& field) {
					const auto text = std::get<1>(field);
					if (boost::regex_search(text, boost::wregex(L"^to|^from", boost::regex::icase)))
						return true;
					if (boost::regex_search(text, boost::wregex(L"\\s+to:", boost::regex::icase)))
						return false;
					return true;
				});

				std::vector<std::pair<cv::Rect, std::wstring>> port_name;

				if (category == L"LETTER OF CREDIT" || category == L"INSURANCE POLICY" || category == L"CERTIFICATE") {
					port_name = extract_field_values(filtered_fields, blocks,
						search_self,
						std::bind(&trade_document_recognizer::preprocess_port_name,
						std::placeholders::_1,
						configuration, L"countries"),
						create_extract_function(configuration, L"ports", 2),
						postprocess_uppercase);

					if (port_name.empty()) {
						port_name = extract_field_values(filtered_fields, blocks,
							search_self,
							std::bind(&trade_document_recognizer::preprocess_country_port_name,
							std::placeholders::_1,
							configuration, L"countries", false),
							create_extract_function(L"(?:ANY (.*) PORT|ANY PORT IN (.*))"),
							create_postprocess_function(configuration, L"country-countrycode2.csv"));
					}

					if (port_name.empty()) {
						port_name = extract_field_values(filtered_fields, blocks,
							std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
							std::bind(&trade_document_recognizer::preprocess_country_port_name,
							std::placeholders::_1,
							configuration, L"countries", true),
							default_extract,
							create_postprocess_function(configuration, L"country-countrycode2.csv"));
					}

					if (port_name.empty()) {
						port_name = extract_field_values(filtered_fields, blocks,
							std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
							std::bind(&trade_document_recognizer::preprocess_port_name,
							std::placeholders::_1, configuration, L"countries"),
							create_extract_function(configuration, L"ports", 2),
							postprocess_uppercase);
					}

					if (port_name.empty()) {
						port_name = extract_field_values(filtered_fields, blocks,
							std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
							std::bind(&trade_document_recognizer::preprocess_country_port_name,
							std::placeholders::_1,
							configuration, L"countries", false),
							create_extract_function(L"(?:ANY (.*) PORT|ANY PORT IN (.*))"),
							create_postprocess_function(configuration, L"country-countrycode2.csv"));
					}

					if (port_name.empty()) {
						if (category == L"INSURANCE POLICY") {
							port_name = extract_field_values(filtered_fields, blocks,
								std::bind(find_nearest_down_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false, false, false),
								std::bind(&trade_document_recognizer::preprocess_country_port_name,
								std::placeholders::_1,
								configuration, L"countries", true),
								default_extract,
								create_postprocess_function(configuration, L"country-countrycode2.csv"));

							if (port_name.empty()) {
								port_name = extract_field_values(filtered_fields, blocks,
									std::bind(find_nearest_down_lines, std::placeholders::_1, std::placeholders::_2, 0.01, 0.25, 2.5, false, false, false),
									std::bind(&trade_document_recognizer::preprocess_port_name,
									std::placeholders::_1,
									configuration, L"countries"),
									create_extract_function(configuration, L"ports", 2),
									postprocess_uppercase);
							}

							if (port_name.empty()) {
								port_name = extract_field_values(filtered_fields, blocks,
									std::bind(find_nearest_down_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false, false, false),
									std::bind(&trade_document_recognizer::preprocess_country_port_name,
									std::placeholders::_1,
									configuration, L"countries", false),
									create_extract_function(L"(?:ANY (.*) PORT|ANY PORT IN (.*))"),
									create_postprocess_function(configuration, L"country-countrycode2.csv"));
							}
						}
					}
				}
				else if (category == L"COMMERCIAL INVOICE" || category == L"PACKING LIST" || category == L"SHIPMENT ADVICE") {
					port_name = extract_field_values(filtered_fields, blocks,
						search_self,
						std::bind(&trade_document_recognizer::preprocess_port_name,
						std::placeholders::_1,
						configuration, L"countries"),
						create_extract_function(configuration, L"ports", 2),
						postprocess_uppercase);
					if (port_name.empty()) {
						port_name = extract_field_values(filtered_fields, blocks,
							std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
							std::bind(&trade_document_recognizer::preprocess_port_name,
							std::placeholders::_1,
							configuration, L"countries"),
							create_extract_function(configuration, L"ports", 2),
							postprocess_uppercase);
					}
					if (port_name.empty()) {
						port_name = extract_field_values(filtered_fields, blocks,
							std::bind(find_nearest_down_lines, std::placeholders::_1, std::placeholders::_2, 0.2, 0.0, 100, false, false, false),
							std::bind(&trade_document_recognizer::preprocess_port_name,
							std::placeholders::_1,
							configuration, L"countries"),
							create_extract_function(configuration, L"ports", 2),
							postprocess_uppercase);
					}
				}
				else if (category == L"BILL OF LADING" || category == L"CARGO RECEIPT") {
					port_name = extract_field_values(filtered_fields, blocks,
						std::bind(find_nearest_down_lines, std::placeholders::_1, std::placeholders::_2, 0.2, 0.0, 100, false, false, false),
						std::bind(&trade_document_recognizer::preprocess_port_name,
						std::placeholders::_1,
						configuration, L"countries"),
						create_extract_function(configuration, L"ports", 2),
						postprocess_uppercase);
				}
				else if (category == L"AIR WAYBILL") {
					port_name = extract_field_values(filtered_fields, blocks,
						std::bind(find_nearest_down_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false, false, false),
						std::bind(&trade_document_recognizer::preprocess_port_name,
						std::placeholders::_1,
						configuration, L"countries"),
						create_extract_function(configuration, L"ports", 2),
						postprocess_uppercase);
				}
				else if (category == L"LETTER OF GUARANTEE") {
					port_name = extract_field_values(filtered_fields, blocks,
						std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.25, 1.0, 100, true),
						std::bind(&trade_document_recognizer::preprocess_port_name,
						std::placeholders::_1,
						configuration, L"countries"),
						create_extract_function(configuration, L"ports", 2),
						postprocess_uppercase);
				}
				else {
					port_name = extract_field_values(filtered_fields, blocks,
						std::bind(find_nearest_down_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false, false, false),
						std::bind(&trade_document_recognizer::preprocess_port_name,
						std::placeholders::_1,
						configuration, L"countries"),
						create_extract_function(configuration, L"ports", 2),
						postprocess_uppercase);

					if (port_name.empty()) {
						port_name = extract_field_values(filtered_fields, blocks,
							std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
							std::bind(&trade_document_recognizer::preprocess_port_name,
							std::placeholders::_1,
							configuration, L"countries"),
							create_extract_function(configuration, L"ports", 2),
							postprocess_uppercase);
					}
				}

				if (port_name.empty())
					return L"";

				const auto extracted_port_name = boost::to_upper_copy(std::get<1>(port_name[0]));
				auto country_code = create_postprocess_function(configuration, L"port-countrycode.csv")(extracted_port_name);
				if (country_code.empty())
					country_code = create_postprocess_function(configuration, L"country-countrycode.csv")(extracted_port_name);

				//return fmt::format(L"{},{}", extracted_port_name, country_code);
				return fmt::format(L"{}", extracted_port_name);
			}

			static std::wstring
				extract_airport_name(const configuration& configuration, const std::wstring& category, const std::wstring& field_name,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
				const std::vector<block>& blocks)
			{
				if (fields.find(field_name) == fields.end())
					return L"";

				std::vector<std::pair<cv::Rect, std::wstring>> port_name;

				if (category == L"AIR WAYBILL") {
					port_name = extract_field_values(fields.at(field_name), blocks,
						std::bind(find_nearest_down_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false, false, false),
						std::bind(&trade_document_recognizer::preprocess_airport_name,
						std::placeholders::_1,
						configuration, L"countries"),
						create_extract_function(configuration, L"airports", 2),
						postprocess_uppercase);
				}

				if (port_name.empty())
					return L"";

				const auto extracted_port_name = std::get<1>(port_name[0]);
				const auto country_code = create_postprocess_function(configuration, L"airport-countrycode.csv")(extracted_port_name);
				return fmt::format(L"{},{}", extracted_port_name, country_code);
			}

			static std::vector<std::wstring>
				extract_port_of_loading(const configuration& configuration, const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>&
				fields,
				const std::vector<block>& blocks, const cv::Size& image_size)
			{
				std::vector<std::wstring> extracted_result;

				std::vector<std::pair<cv::Rect, std::wstring>> result;

				//if (category == L"BILL OF LADING" ) {
					//if (left_side_field.find(L"PORT OF LOADING") != left_side_field.end())
					result = extract_field_values(fields.at(L"PORT OF LOADING"), blocks,
						std::bind(find_nearest_down_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false, false, false),
						preprocess_port_of_loading,
						default_extract,
						postprocess_uppercase);
				//}

				
				if (result.empty() || to_wstring(result[0]).length() > 50) {
					return extracted_result;
				}
				else {
					//for (auto& a : result) {
					extracted_result.emplace_back(to_wstring(result[0]));
					//}
				}

				return extracted_result;
			}

			static std::vector<std::wstring>
				extract_shipment_date(const configuration& configuration, const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>&
				fields,
				const std::vector<block>& blocks, const cv::Size& image_size)
			{
				std::vector<std::wstring> extracted_result;

				std::vector<std::pair<cv::Rect, std::wstring>> result;

				result = extract_field_values(fields.at(L"ON BOARD DATE"), blocks,
					std::bind(find_down_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
					default_preprocess,
					default_extract,
					postprocess_uppercase);

				if (result.empty() || to_wstring(result[0]).length() > 50) {
					return extracted_result;
				}
				else {
					//for (auto& a : result) {
					extracted_result.emplace_back(to_wstring(result[0]));
					//}
				}

				return extracted_result;
			}

			static std::vector<std::wstring>
				extract_swift_mt_sender( const configuration& configuration, 
										const std::wstring& category,
										const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>&	fields, 
										const std::vector<block>& blocks, 
										const cv::Size& image_size)
			{
				std::vector<std::wstring> extracted_result;
				

				std::vector<std::pair<cv::Rect, std::wstring>> result;

				
				result = extract_field_values(fields.at(L"SWIFT MT SENDER"), blocks,
					search_self,
					preprocess_sender,
					//create_extract_sender(L"SWIFT MT SENDER"),
					default_extract,
					postprocess_uppercase);
				
				if (!result.empty() ) {
					for (auto& a : result) {
						auto& str = boost::algorithm::trim_copy(boost::to_upper_copy(to_wstring(a)));

						if (str.find(L"SWIFT MT SENDER") != std::wstring::npos) {
							str = boost::replace_all_copy(str, L"SWIFT MT SENDER", L"");
							if (str.size() <= 0) {
								result.clear();
							}
							else {
								result.clear();
								extracted_result.emplace_back(boost::replace_all_copy(str, L"SWIFT MT SENDER", L""));
							}
							break;
						}
						
					}
				}
				//extracted_result = find(result.begin(), result.end(), L"SWIFT MT SENDER");
				
				if (result.empty() && extracted_result.empty()) {
					result = extract_field_values(fields.at(L"SWIFT MT SENDER"), blocks,
						std::bind(find_right_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						preprocess_bank_name,
						default_extract,
						postprocess_uppercase);
					
					if (result.empty()) {
						result = extract_field_values(fields.at(L"SWIFT MT SENDER"), blocks,
							std::bind(find_down_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
							preprocess_bank_name,
							default_extract,
							postprocess_uppercase);
					}
				}


				
				if (result.empty()) {
					return extracted_result;
				}
				else {
					for (auto& a : result) {
						extracted_result.emplace_back(to_wstring(a));
					}

					if (extracted_result.size() > 5) {
						for (auto i = 0; i < extracted_result.size(); i++) {
							auto& str = boost::algorithm::trim_copy(boost::to_upper_copy(extracted_result[i]));
							if (str.find(L"BEHEFICIARY") != std::wstring::npos ||
								str.find(L"BENEFICIARY") != std::wstring::npos ||
								str.find(L"BENEBWWT") != std::wstring::npos) {
								extracted_result.resize(i);
								break;
							}
						}
					}
				}
				
				return extracted_result;
			}

			static std::vector<std::wstring>
				extract_credit_number(const configuration& configuration,
									const std::wstring& category,
									const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>&	fields,
									const std::vector<block>& blocks,
									const cv::Size& image_size)
			{

				std::vector<std::wstring> extracted_result;
				std::vector<std::pair<cv::Rect, std::wstring>> result;


				result = extract_field_values(fields.at(L"DOCUMENTARY CREDIT NUMBER"), blocks,
					search_self,
					preprocess_amount,
					default_extract,
					postprocess_uppercase);
				if (result.empty()) {
					result = extract_field_values(fields.at(L"DOCUMENTARY CREDIT NUMBER"), blocks,
						std::bind(find_right_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						preprocess_amount,
						default_extract,
						postprocess_uppercase);

					if (result.empty()) {
						result = extract_field_values(fields.at(L"DOCUMENTARY CREDIT NUMBER"), blocks,
							std::bind(find_down_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
							preprocess_amount,
							default_extract,
							postprocess_uppercase);
					}
				}

				if (result.empty() || to_wstring(result[0]).length() > 50) {
					return extracted_result;
				}
				else {
					//for (auto& a : result) {
					extracted_result.emplace_back(to_wstring(result[0]));
					//}
				}

				return extracted_result;
			}

			static std::vector<std::wstring>
				extract_place_of_receipt(const configuration& configuration, const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>&
				fields,
				const std::vector<block>& blocks, const cv::Size& image_size)
			{
				std::vector<std::wstring> extracted_result;

				std::vector<std::pair<cv::Rect, std::wstring>> result;

				result = extract_field_values(fields.at(L"PLACE OF RECEIPT"), blocks,
					std::bind(find_down_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
					default_preprocess,
					default_extract,
					postprocess_uppercase);

				if (result.empty() || to_wstring(result[0]).length() > 50) {
					return extracted_result;
				}
				else {
					//for (auto& a : result) {
					extracted_result.emplace_back(to_wstring(result[0]));
					//}
				}

				return extracted_result;
			}

			static std::vector<std::wstring>
				extract_port_of_discharge(const configuration& configuration, const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>&
				fields,
				const std::vector<block>& blocks, const cv::Size& image_size)
			{
				std::vector<std::wstring> extracted_result;

				std::vector<std::pair<cv::Rect, std::wstring>> result;

				result = extract_field_values(fields.at(L"PORT OF DISCHARGE"), blocks,
					std::bind(find_nearest_down_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false, false, false),
					//std::bind(&trade_document_recognizer::preprocess_port_of_discharge, std::placeholders::_1, configuration, L"PORT OF DISCHARGE"),
					//default_preprocess,
					preprocess_port_of_discharge,
					default_extract,
					postprocess_discharge);

				if (result.empty()) {
					result = extract_field_values(fields.at(L"PORT OF DISCHARGE"), blocks,
						std::bind(find_down_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						preprocess_port_of_discharge,
						default_extract,
						postprocess_discharge);
//					return extracted_result;
				}
				else if (to_wstring(result[0]).length() > 50) {
					return extracted_result;
				} else {
					//for (auto& a : result) {
					extracted_result.emplace_back(to_wstring(result[0]));
					//}
				}

				if (result.empty()) {
					return extracted_result;
				}

				return extracted_result;
			}

			static std::vector<std::wstring>
				extract_place_of_delivery(const configuration& configuration, const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>&
				fields,
				const std::vector<block>& blocks, const cv::Size& image_size)
			{
				std::vector<std::wstring> extracted_result;

				std::vector<std::pair<cv::Rect, std::wstring>> result;

				result = extract_field_values(fields.at(L"PLACE OF DELIVERY"), blocks,
					//std::bind(find_nearest_down_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false, false, false),
					std::bind(find_down_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
					preprocess_port_of_discharge,
					default_extract,
					postprocess_discharge);

				if (result.empty() || to_wstring(result[0]).length() > 50) {
					return extracted_result;
				}
				else {
					//for (auto& a : result) {
					extracted_result.emplace_back(to_wstring(result[0]));
					//}
				}

				return extracted_result;

			}

			static bool
				find_company(const std::pair<cv::Rect, std::wstring>& a, const configuration& configuration, const std::wstring& category)
			{
				auto text = std::get<1>(a);
				text = boost::regex_replace(text, boost::wregex(L"[^a-zA-Z\\., ]"), L"");
				text = boost::regex_replace(text, boost::wregex(L"(\\s)\\s+"), L"$1");
				text = boost::regex_replace(text, boost::wregex(L"\\s*([\\.,])\\s*"), L"$1");
				text = boost::regex_replace(text, boost::wregex(L"\\s+(\\S)\\s+"), L"$1 ");
				text = boost::regex_replace(text, boost::wregex(L"\\s+TO", boost::regex::icase), L"");

				std::vector<std::wstring> words;
				boost::algorithm::split(words, text, boost::is_any_of(L" ."), boost::token_compress_on);

				const auto keywords = get_dictionary_words(configuration, dictionaries_, category);
				const auto dictionary = build_spell_dictionary(configuration, category);
				aho_corasick::wtrie trie;
				build_trie(trie, keywords);

				if (words.size() > 1) {
					for (auto& word : words) {
						if (word.find(L'\n') == std::wstring::npos) {
							const auto suggested = dictionary->Correct(word);
							if (!suggested.empty() && suggested[0].distance <= get_distance_threshold(word))
								word = suggested[0].term;
						}
						else {
							std::vector<std::wstring> cleaned_words;
							boost::algorithm::split(cleaned_words, word, boost::is_any_of(L"\n"));
							for (auto& word : cleaned_words) {
								const auto suggested = dictionary->Correct(word);
								if (!suggested.empty() && suggested[0].distance <= 1)
									word = suggested[0].term;
							}
							word = boost::algorithm::join(cleaned_words, L"\n");
						}
					};

					text = boost::algorithm::join(words, L" ");
				}

				const auto matches = trie.parse_text(text);

				if (!matches.empty()) {
					return true;
				}

				return false;
			}

			/*
			static std::vector<std::wstring>
				extract_lcno(const configuration& configuration, const std::wstring& category,
					const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
					const std::vector<block>& blocks, const cv::Size& image_size)
			{
				std::vector<std::wstring> extracted_lcno;

				auto result = extract_field_values(fields.at(L"L/C NO"), blocks,
						std::bind(find_down_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						default_preprocess, 
						default_extract, 
						default_postprocess);

				if (result.empty())
					return std::vector<std::wstring>();
				
				for (auto& a : result) {
					extracted_lcno.emplace_back(to_wstring(a));
				}

				return extracted_lcno;

				//return extract_company_and_address(configuration, category, L"L/C NO", fields, blocks);
			}
			*/
			static std::wstring
				extract_lc_number(const configuration& configuration, const std::wstring& category,
					const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
					const std::vector<block>& blocks, const cv::Size& image_size)
			{
				if (fields.find(L"L/C NO") == fields.end())
					return L"";

				std::vector<std::pair<cv::Rect, std::wstring>> lc_number;

				if (category == L"COMMERCIAL INVOICE") {
					lc_number = extract_field_values(fields.at(L"L/C NO"), blocks,
						search_self,
						preprocess_lc_number,
						default_extract,
						std::bind(&selvy::ocr::trade_document_recognizer::postprocess_lc_number, std::placeholders::_1, configuration));

					if (lc_number.empty()) {
						lc_number = extract_field_values(fields.at(L"L/C NO"), blocks,
							std::bind(find_nearest_down_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false, false, false),
							preprocess_lc_number,
							default_extract,
							std::bind(&selvy::ocr::trade_document_recognizer::postprocess_lc_number, std::placeholders::_1, configuration));
						//std::bind(&selvy::ocr::trade_document_recognizer::postprocess_lc_number, std::placeholders::_1, configuration));
					}
					if (lc_number.empty())
						lc_number = extract_field_values(fields.at(L"L/C NO"), blocks,
							std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 5.0, 8, false),
							preprocess_lc_number,
							default_extract,
							std::bind(&selvy::ocr::trade_document_recognizer::postprocess_lc_number, std::placeholders::_1, configuration));
				}


				if (lc_number.empty())
					return L"";

				return std::get<1>(lc_number[0]);
			}

			static std::wstring
				extract_license_number(const configuration& configuration, const std::wstring& category,
					const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
					const std::vector<block>& blocks, const cv::Size& image_size, const std::wstring& search_field_name)
			{
				if (fields.find(search_field_name) == fields.end())
					return L"";

				std::vector<std::pair<cv::Rect, std::wstring>> found;
				//categoty == ""
				if (true) {
					found = extract_field_values(fields.at(search_field_name), blocks,
						search_self,
						default_preprocess,
						default_extract,
						std::bind(&selvy::ocr::trade_document_recognizer::postprocess_license_number, std::placeholders::_1, configuration));

					if (found.empty()) {
						found = extract_field_values(fields.at(search_field_name), blocks,
							std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
							default_preprocess,
							default_extract,
							std::bind(&selvy::ocr::trade_document_recognizer::postprocess_license_number, std::placeholders::_1, configuration));
						//std::bind(&selvy::ocr::trade_document_recognizer::postprocess_lc_number, std::placeholders::_1, configuration));
						//postprecess_license_number 수정필요
					}

				}


				if (found.empty())
					return L"";

				return std::get<1>(found[0]);
			}

			//tyler
			static std::wstring
				extract_currency_sign(const configuration& configuration, const std::wstring& category,
					const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
					const std::vector<block>& blocks, const cv::Size& image_size)
			{
				if (fields.find(L"CURRENCY SIGN") == fields.end())
					return L"";

				std::vector<std::pair<cv::Rect, std::wstring>> currency_sign;

				if (category == L"COMMERCIAL INVOICE") {
					currency_sign = extract_field_values(fields.at(L"CURRENCY SIGN"), blocks,
						search_self,
						preprocess_currency_sign,
						default_extract,
						std::bind(&selvy::ocr::trade_document_recognizer::postprocess_currency_sign, std::placeholders::_1, configuration));

					if (currency_sign.empty())
						currency_sign = extract_field_values(fields.at(L"CURRENCY SIGN"), blocks,
							std::bind(find_down_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
							preprocess_currency_sign,
							default_extract,
							std::bind(&selvy::ocr::trade_document_recognizer::postprocess_currency_sign, std::placeholders::_1, configuration));

					if (currency_sign.empty())
						currency_sign = extract_field_values(fields.at(L"CURRENCY SIGN"), blocks,
							std::bind(find_right_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
							preprocess_currency_sign,
							default_extract,
							std::bind(&selvy::ocr::trade_document_recognizer::postprocess_currency_sign, std::placeholders::_1, configuration));
				}


				if (currency_sign.empty())
					return L"";

				return std::get<1>(currency_sign[0]);
			}

			//find total line
			static std::wstring
				extract_amount(const configuration& configuration, const std::wstring& category,
					const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
					const std::vector<block>& blocks, const cv::Size& image_size)
			{
				if (fields.find(L"AMOUNT") == fields.end())
					return L"";

				std::vector<std::pair<cv::Rect, std::wstring>> total;

				if (category == L"COMMERCIAL INVOICE") {
					total = extract_field_values2(fields.at(L"AMOUNT"), blocks,
						search_self,
						preprocess_amount,
						default_extract,
						std::bind(&selvy::ocr::trade_document_recognizer::postprocess_amount, std::placeholders::_1, configuration));

					if (total.empty())
						total = extract_field_values2(fields.at(L"AMOUNT"), blocks,
							std::bind(find_right_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 5.0, 8, false),
							preprocess_amount,
							default_extract,
							std::bind(&selvy::ocr::trade_document_recognizer::postprocess_amount, std::placeholders::_1, configuration));
				} else if (category == L"LETTER OF CREDIT") {
					total = extract_field_values(fields.at(L"CURRENCY CODE AMOUNT"), blocks,
						search_self,
						preprocess_amount,
						default_extract,
						default_postprocess);

					if (total.empty()) {
						total = extract_field_values(fields.at(L"CURRENCY CODE AMOUNT"), blocks,
							std::bind(find_right_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
							preprocess_amount,
							default_extract,
							default_postprocess);
					}

				}


				if (total.empty())
					return L"";

				return std::get<1>(total[0]);
			}
			/*
			static std::vector<std::wstring>
				extract_amount(const configuration& configuration, const std::wstring& category,
					const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
					const std::vector<block>& blocks, const cv::Size& image_size)
			{
				std::vector<std::wstring> extracted_result;
				std::vector<std::pair<cv::Rect, std::wstring>> result;
				auto isAmount = false;

				if (category == L"LETTER OF CREDIT") {
					result = extract_field_values(fields.at(L"CURRENCY CODE AMOUNT"), blocks,
						search_self,
						preprocess_amount,
						default_extract,
						default_postprocess);

					if (result.empty()) {
						result = extract_field_values(fields.at(L"CURRENCY CODE AMOUNT"), blocks,
							std::bind(find_right_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
							preprocess_amount,
							default_extract,
							default_postprocess);
					}
					
				} else if (category == L"COMMERCIAL INVOICE") {
					result = extract_field_values(fields.at(L"CURRENCY CODE AMOUNT"), blocks,
						std::bind(find_down_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						preprocess_amount,
						default_extract,
						default_postprocess);
				}
				

				if (result.empty())
					return std::vector<std::wstring>();

				//for (auto& a : result) {
					extracted_result.emplace_back(to_wstring(result[0]));
				//}

				return extracted_result;

				//return extract_company_and_address(configuration, category, L"L/C NO", fields, blocks);
			}
			*/

			static bool
				find_address_country(const std::pair<cv::Rect, std::wstring>& a, const configuration& configuration, const std::wstring& category, const int line_index)
			{
				auto text = std::get<1>(a);
				text = boost::regex_replace(text, boost::wregex(L"[^a-zA-Z\\.,]"), L"");
				text = boost::regex_replace(text, boost::wregex(L"(\\s)\\s+"), L"$1");
				text = boost::regex_replace(text, boost::wregex(L"\\s*([\\.,])\\s*"), L"$1");
				text = boost::regex_replace(text, boost::wregex(L"\\s+(\\S)\\s+"), L"$1 ");
				text = boost::regex_replace(text, boost::wregex(L"\\s+TO", boost::regex::icase), L"");

				std::vector<std::wstring> words;
				boost::algorithm::split(words, text, boost::is_any_of(L" ."), boost::token_compress_on);

				const auto keywords = get_dictionary_words(configuration, dictionaries_, category);
				const auto dictionary = build_spell_dictionary(configuration, category);
				aho_corasick::wtrie trie;
				build_trie(trie, keywords);

				if (words.size() > 1) {
					for (auto& word : words) {
						if (word.find(L'\n') == std::wstring::npos) {
							const auto suggested = dictionary->Correct(word);
							if (!suggested.empty() && suggested[0].distance <= get_distance_threshold(word))
								word = suggested[0].term;
						}
						else {
							std::vector<std::wstring> cleaned_words;
							boost::algorithm::split(cleaned_words, word, boost::is_any_of(L"\n"));
							for (auto& word : cleaned_words) {
								const auto suggested = dictionary->Correct(word);
								if (!suggested.empty() && suggested[0].distance <= 1)
									word = suggested[0].term;
							}
							word = boost::algorithm::join(cleaned_words, L"\n");
						}
					};

					text = boost::algorithm::join(words, L" ");
				}

				const auto matches = trie.parse_text(text);

				if (!matches.empty()) {
					if (line_index > 1) {
						return true;
					}
					else {
						for (const auto& match : matches) {
							if (match.get_end() + 1 == text.size() || match.get_end() + 2 == text.size())
								return true;
						}
					}
				}

				return false;
			}

			static std::vector<std::wstring>
				extract_consignee(const configuration& configuration, const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
				const std::vector<block>& blocks, const cv::Size& image_size)
			{
				if (fields.find(L"CONSIGNEE") == fields.end())
					return std::vector<std::wstring>();

				std::vector<std::wstring> extracted_consignee;

				std::vector<std::pair<cv::Rect, std::wstring>> consignee;

				const auto paper = image_size;
				std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>> left_side_field;
				for (auto& field : fields.at(L"CONSIGNEE")) {
					auto rect = to_rect(field);

					if (rect.x < paper.width / 4 && rect.y < paper.height / 2) {
						std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>> new_field;
						new_field.emplace_back(field);
						left_side_field.insert(std::make_pair(L"CONSIGNEE", new_field));
						break;
					}
				}

				if (category == L"BILL OF LADING" || category == L"CARGO RECEIPT") {
					if (left_side_field.find(L"CONSIGNEE") != left_side_field.end())
						consignee = extract_field_values(left_side_field.at(L"CONSIGNEE"), blocks,
						std::bind(find_nearest_down_lines, std::placeholders::_1, std::placeholders::_2, 0.2, 0.0, 10, true, true, false),
						default_preprocess,
						//create_extract_function(L"(?:ORDER OF )(.*)"),
						default_extract,
						postprocess_uppercase);
				}
				else if (category == L"LETTER OF GUARANTEE") {
					consignee = extract_field_values(fields.at(L"CONSIGNEE"), blocks,
						std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.25, 1.0, 10, true),
						default_preprocess,
						create_extract_function(L"(?:ORDER OF )(.*)"),
						postprocess_uppercase);

					if (!consignee.empty()) {
						const auto same_blocks = find_nearest_down_lines(to_block(consignee[0]), blocks, 0.5, 0.0, 2, true);
						std::transform(std::begin(same_blocks), std::end(same_blocks), std::back_inserter(consignee), [](const std::pair<cv::Rect, std::wstring>& line) {
							return std::make_pair(std::get<0>(line), postprocess_uppercase(std::get<1>(line)));
						});
					}
				}
				else if (category == L"COMMERCIAL INVOICE" || category == L"PACKING LIST") {
					consignee = extract_field_values(fields.at(L"CONSIGNEE"), blocks,
						std::bind(find_nearest_down_lines, std::placeholders::_1, std::placeholders::_2, 0.2, 0.0, 10, true, true, false),
						default_preprocess,
						default_extract,	//create_extract_function(L"(?:ORDER OF )(.*)"),
						postprocess_uppercase);

					if (!consignee.empty()) {
						consignee = extract_field_values(fields.at(L"CONSIGNEE"), blocks,
							search_self,
							std::bind(&trade_document_recognizer::preprocess_company, std::placeholders::_1,
							configuration, L"companies"),
							default_extract,
							postprocess_uppercase);
					}
				}
				else {
					consignee = extract_field_values(fields.at(L"CONSIGNEE"), blocks,
						std::bind(find_nearest_down_lines, std::placeholders::_1, std::placeholders::_2, 0.2, 0.0, 10, true, true, false),
						default_preprocess,
						create_extract_function(L"(?:ORDER OF )(.*)"),
						postprocess_uppercase);
				}

				if (consignee.empty()) {
					if (category == L"BILL OF LADING" || category == L"CARGO RECEIPT") {
						extracted_consignee = extract_company_and_address(configuration, category, L"CONSIGNEE", left_side_field, blocks);
						if (!extracted_consignee.empty() && extracted_consignee.size() > 5) {
							for (auto i = 0; i < extracted_consignee.size(); i++) {
								auto& str = boost::algorithm::trim_copy(boost::to_upper_copy(extracted_consignee[i]));
								if (str.find(L"NOTIFY PARTY") != std::wstring::npos) {
									extracted_consignee.resize(i);
									break;
								}
							}
						}
					}
					else {
						extracted_consignee = extract_company_and_address(configuration, category, L"CONSIGNEE", fields, blocks);
					}
				}
				else {
					for (auto& a : consignee) {
						extracted_consignee.emplace_back(to_wstring(a));
					}
				}

				return extracted_consignee;
			}

			static std::vector<std::wstring>
				extract_shipper(const configuration& configuration, const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
				const std::vector<block>& blocks, const cv::Size& image_size)
			{
				if (fields.find(L"SHIPPER") == fields.end())
					return std::vector<std::wstring>();
				if (fields.at(L"SHIPPER").size() > 0) {
					const auto paper = image_size;
					std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>> left_side_field;
					for (auto& field : fields.at(L"SHIPPER")) {
						auto rect = to_rect(field);
						if (rect.x < paper.width / 4 && rect.y < paper.height / 2) {
							std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>> new_field;
							new_field.emplace_back(field);
							left_side_field.insert(std::make_pair(L"SHIPPER", new_field));
							break;
						}
					}
					if (left_side_field.find(L"SHIPPER") == left_side_field.end())
						return std::vector<std::wstring>();

					return extract_company_and_address(configuration, category, L"SHIPPER", left_side_field, blocks);
				}
				return extract_company_and_address(configuration, category, L"SHIPPER", fields, blocks);
			}

			static std::vector<std::wstring>
				extract_shipping_company(const configuration& configuration, const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
				const std::vector<block>& blocks, const cv::Size& image_size)
			{
				if (fields.find(L"CARRIER") == fields.end())
					return std::vector < std::wstring>();
				if (fields.at(L"CARRIER").size() > 0) {
					const auto paper = image_size;
					std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>> left_side_field;
					for (auto& field : fields.at(L"CARRIER")) {
						auto rect = to_rect(field);
						if (rect.x < paper.width / 4 && rect.y < paper.height / 2) {
							std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>> new_field;
							new_field.emplace_back(field);
							left_side_field.insert(std::make_pair(L"CARRIER", new_field));
							break;
						}
					}
					if (left_side_field.find(L"CARRIER") == left_side_field.end())
						return std::vector<std::wstring>();

					return extract_company_and_address(configuration, category, L"CARRIER", left_side_field, blocks);
				}
				return extract_company_and_address(configuration, category, L"CARRIER", fields, blocks);
			}

			static std::vector<std::wstring>
				extract_notify(const configuration& configuration, const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
				const std::vector<block>& blocks, const cv::Size& image_size)
			{
				const auto FIELD_NAME = L"NOTIFY";

				std::vector<std::wstring> extracted_result;

				std::vector<std::pair<cv::Rect, std::wstring>> result;

				const auto paper = image_size;
				std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>> left_side_field;
				for (auto& field : fields.at(FIELD_NAME)) {
					auto rect = to_rect(field);

					if (rect.x < paper.width / 4 && rect.y < paper.height / 2) {
						std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>> new_field;
						new_field.emplace_back(field);
						left_side_field.insert(std::make_pair(FIELD_NAME, new_field));
						break;
					}
				}

				if (category == L"BILL OF LADING") {
					if (left_side_field.find(FIELD_NAME) != left_side_field.end())
						result = extract_field_values(left_side_field.at(FIELD_NAME), blocks,
							std::bind(find_down_lines, std::placeholders::_1, std::placeholders::_2, 0.2, 0.0, 10, true),
							default_preprocess,
							//create_extract_function(L"(?:ORDER OF )(.*)"),
							default_extract,
							postprocess_uppercase);
				}
				else {
					result = extract_field_values(fields.at(FIELD_NAME), blocks,
						std::bind(find_nearest_down_lines, std::placeholders::_1, std::placeholders::_2, 0.2, 0.0, 10, true, true, false),
						default_preprocess,
						create_extract_function(L"(?:ORDER OF )(.*)"),
						postprocess_uppercase);
				}

				if (result.empty()) {
					if (category == L"BILL OF LADING" ) {
						extracted_result = extract_company_and_address(configuration, category, FIELD_NAME, left_side_field, blocks);
						if (!extracted_result.empty() && extracted_result.size() > 5) {
							for (auto i = 0; i < extracted_result.size(); i++) {
								auto& str = boost::algorithm::trim_copy(boost::to_upper_copy(extracted_result[i]));
								if (str.find(FIELD_NAME) != std::wstring::npos) {
									extracted_result.resize(i);
									break;
								}
							}
						}
					}
					else {
						extracted_result = extract_company_and_address(configuration, category, FIELD_NAME, fields, blocks);
					}
				}
				else {
					for (auto& a : result) {
						extracted_result.emplace_back(to_wstring(a));
					}
				}

				return extracted_result;
			}

			static std::vector<std::wstring>
				extract_agent(const configuration& configuration, const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
				const std::vector<block>& blocks, const cv::Size& image_size)
			{
				std::set<std::wstring> agents;

				if (fields.find(L"AGENT") == fields.end())
					return std::vector<std::wstring>();

				if (category == L"AIR WAYBILL" || category == L"BILL OF LADING") {
					std::vector<std::wstring> company_and_addresses;

					if (fields.at(L"AGENT").size() > 0) {
						const auto paper = image_size;
						std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>> new_field;
						for (auto& field : fields.at(L"AGENT")) {
							auto rect = to_rect(field);
							if (rect.y > paper.height * 3 / 4) {
								new_field.emplace_back(field);
							}
						}

						std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>> down_side_field;
						down_side_field.insert(std::make_pair(L"AGENT", new_field));

						if (down_side_field.find(L"AGENT") != down_side_field.end()) {
							auto agent = extract_field_values(down_side_field.at(L"AGENT"), blocks,
								search_line,
								default_preprocess,
								create_extract_function(L"(?:BY )?(.*) AS AGENTS?(?: FOR)?"),
								postprocess_uppercase);

							if (!agent.empty()) {
								agents.emplace(std::get<1>(agent[0]));
							}
							else {
								const auto regex = L"^AS AGENTS?(?: FOR) ?";
								boost::wregex re(regex, boost::regex_constants::icase);
								boost::match_results<std::wstring::const_iterator> matches;

								for (auto i = 0; i < down_side_field.at(L"AGENT").size(); i++) {
									const auto agent_field = down_side_field.at(L"AGENT")[i];

									const auto text = std::get<1>(agent_field);
									if (boost::regex_search(std::begin(text), std::end(text), matches, re)) {
										const auto suspected_agents = find_nearest_up_lines(agent_field, blocks, 0.2, 2);

										if (suspected_agents.empty())
											continue;

										const auto agent = preprocess_company(suspected_agents[0], configuration, L"companies");
										if (!std::get<1>(agent).empty()) {
											agents.emplace(postprocess_uppercase(std::get<1>(agent)));
											break;
										}
									}
								}
							}
						}
					}

					if (company_and_addresses.empty())
						company_and_addresses = extract_company_and_address(configuration, category, L"AGENT", fields, blocks);

					if (!company_and_addresses.empty()) {
						boost::remove_erase_if(company_and_addresses, [](const std::wstring& addresses) {
							return addresses.find(L"IATA CODE") != std::wstring::npos;
						});

						agents.emplace(company_and_addresses[0]);
					}
					std::set<std::wstring> new_agents;
					for (auto& agent : agents) {
						auto new_agent = boost::regex_replace(agent, boost::wregex(L"^by ", boost::regex::icase), L"");
						new_agent = boost::regex_replace(new_agent, boost::wregex(L"^as carrier ", boost::regex::icase), L"");
						new_agent = boost::regex_replace(new_agent, boost::wregex(L"^for ", boost::regex::icase), L"");
						new_agent = boost::regex_replace(new_agent, boost::wregex(L"^and ", boost::regex::icase), L"");
						if (!new_agent.empty() && !boost::regex_search(new_agent, boost::wregex(L"^PLACE OF RECEIPT"))
							&& !boost::regex_search(new_agent, boost::wregex(L"^(?:FOR )?THE CARRIER"))
							&& !boost::regex_search(new_agent, boost::wregex(L"^B/L NO"))
							&& !boost::regex_search(new_agent, boost::wregex(L"^FMC NO"))
							&& !boost::regex_search(new_agent, boost::wregex(L"FREIGHT PREPAID")))
							new_agents.emplace(new_agent);
					}
					agents = new_agents;
				}

				return std::vector<std::wstring>{std::begin(agents), std::end(agents)};
			}


			static std::wstring
				extract_vessel_name(const configuration& configuration, const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
				const std::vector<block>& blocks, const cv::Size& image_size)
			{
				if (fields.find(L"VESSEL NAME") == fields.end())
					return L"";

				std::vector<std::pair<cv::Rect, std::wstring>> vessel_name;

				if (category == L"SHIPMENT ADVICE") {
					vessel_name = extract_field_values(fields.at(L"VESSEL NAME"), blocks,
						search_self,
						preprocess_vessel_name,
						default_extract,
						std::bind(&selvy::ocr::trade_document_recognizer::postprocess_vessel_name, std::placeholders::_1, configuration));

					if (vessel_name.empty())
						vessel_name = extract_field_values(fields.at(L"VESSEL NAME"), blocks,
						std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 5.0, 8, false),
						preprocess_vessel_name,
						default_extract,
						std::bind(&selvy::ocr::trade_document_recognizer::postprocess_vessel_name, std::placeholders::_1, configuration));
				}
				else if (category == L"CERTIFICATE") {
					vessel_name = extract_field_values(fields.at(L"VESSEL NAME"), blocks,
						search_self,
						preprocess_vessel_name,
						default_extract,
						std::bind(&selvy::ocr::trade_document_recognizer::postprocess_vessel_name, std::placeholders::_1, configuration));

					if (vessel_name.empty())
						vessel_name = extract_field_values(fields.at(L"VESSEL NAME"), blocks,
						std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 8, false),
						preprocess_vessel_name,
						default_extract,
						std::bind(&selvy::ocr::trade_document_recognizer::postprocess_vessel_name, std::placeholders::_1, configuration));
				}
				else if (category == L"BILL OF LADING" || category == L"CARGO RECEIPT") {
					vessel_name = extract_field_values(fields.at(L"VESSEL NAME"), blocks,
						std::bind(find_nearest_down_lines, std::placeholders::_1, std::placeholders::_2, 0.01, 0.25, 2.5, false, false, false),
						preprocess_vessel_name,
						default_extract,
						std::bind(&selvy::ocr::trade_document_recognizer::postprocess_vessel_name, std::placeholders::_1, configuration));

				}
				else if (category == L"LETTER OF GUARANTEE") {
					vessel_name = extract_field_values(fields.at(L"VESSEL NAME"), blocks,
						std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.25, 1.0, 100, true),
						preprocess_vessel_name,
						default_extract,
						std::bind(&selvy::ocr::trade_document_recognizer::postprocess_vessel_name, std::placeholders::_1, configuration));
				}
				else {
					vessel_name = extract_field_values(fields.at(L"VESSEL NAME"), blocks,
						std::bind(find_nearest_down_lines, std::placeholders::_1, std::placeholders::_2, 0.01, 0.25, 2, false, false, false),
						preprocess_vessel_name,
						default_extract,
						std::bind(&selvy::ocr::trade_document_recognizer::postprocess_vessel_name, std::placeholders::_1, configuration));

					if (vessel_name.empty() && category == L"INSURANCE POLICY") {
						vessel_name = extract_field_values(fields.at(L"VESSEL NAME"), blocks,
							std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 8, false),
							preprocess_vessel_name,
							default_extract,
							std::bind(&selvy::ocr::trade_document_recognizer::postprocess_vessel_name, std::placeholders::_1, configuration));
						if (vessel_name.empty()) {
							vessel_name = extract_field_values(fields.at(L"VESSEL NAME"), blocks,
								search_self,
								preprocess_vessel_name,
								default_extract,
								std::bind(&selvy::ocr::trade_document_recognizer::postprocess_vessel_name, std::placeholders::_1, configuration));
						}
					}
				}

				boost::remove_erase_if(vessel_name, [](const std::pair<cv::Rect, std::wstring>& vessel) {
					return boost::regex_search(std::get<1>(vessel), boost::wregex(L"TO BE (?:DECLARED|ADVISED)", boost::regex_constants::icase)); });

				if (vessel_name.empty())
					return L"";

				return std::get<1>(vessel_name[0]);
			}

			static std::wstring
				extract_goods_description(const configuration& configuration, const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>&
				fields,
				const std::vector<block>& blocks, const cv::Size& image_size)
			{
				if (fields.find(L"GOODS DESCRIPTION") == fields.end())
					return L"";

				std::vector<std::pair<cv::Rect, std::wstring>> good_descriptions;

				if (category == L"BILL OF LADING") {
					good_descriptions = extract_field_values(fields.at(L"GOODS DESCRIPTION"), blocks,
						std::bind(find_down_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						std::bind(&trade_document_recognizer::preprocess_good_description,
						std::placeholders::_1),
						default_extract,
						std::bind(&trade_document_recognizer::postprocess_goods_description, std::placeholders::_1, configuration));

					const auto paper = image_size;
					boost::remove_erase_if(good_descriptions, [&paper](const std::pair<cv::Rect, std::wstring>& block) {
						const auto block_rect = to_rect(block);
						return block_rect.y < paper.height / 2 || block_rect.x > paper.width / 2;
					});
				}
				else if (category == L"INSURANCE POLICY") {
					good_descriptions = extract_field_values(fields.at(L"GOODS DESCRIPTION"), blocks,
						search_self,
						std::bind(&trade_document_recognizer::preprocess_good_description, std::placeholders::_1),
						default_extract,
						std::bind(&trade_document_recognizer::postprocess_goods_description, std::placeholders::_1, configuration));

					if (good_descriptions.empty()) {
						good_descriptions = extract_field_values(fields.at(L"GOODS DESCRIPTION"), blocks,
							std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
							std::bind(&trade_document_recognizer::preprocess_good_description,
							std::placeholders::_1),
							default_extract,
							std::bind(&trade_document_recognizer::postprocess_goods_description, std::placeholders::_1, configuration));
					}

					if (good_descriptions.empty()) {
						good_descriptions = extract_field_values(fields.at(L"GOODS DESCRIPTION"), blocks,
							std::bind(find_nearest_down_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false, false, false),
							std::bind(&trade_document_recognizer::preprocess_good_description,
							std::placeholders::_1),
							default_extract,
							std::bind(&trade_document_recognizer::postprocess_goods_description, std::placeholders::_1, configuration));
					}
				}
				else if (category == L"CERTIFICATE") {
					good_descriptions = extract_field_values(fields.at(L"GOODS DESCRIPTION"), blocks,
						std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						std::bind(&trade_document_recognizer::preprocess_good_description,
						std::placeholders::_1),
						default_extract,
						std::bind(&trade_document_recognizer::postprocess_goods_description, std::placeholders::_1, configuration));
				}
				else if (category == L"LETTER OF GUARANTEE") {
					good_descriptions = extract_field_values(fields.at(L"GOODS DESCRIPTION"), blocks,
						std::bind(find_down_lines, std::placeholders::_1, std::placeholders::_2, 0.25, 0.0, 6, false),
						std::bind(&trade_document_recognizer::preprocess_good_description,
						std::placeholders::_1),
						default_extract,
						std::bind(&trade_document_recognizer::postprocess_goods_description, std::placeholders::_1, configuration));
				}
				else if (category == L"EXPORT PERMIT") {
					good_descriptions = extract_field_values(fields.at(L"GOOD DESCRIPTION"), blocks,
						search_self,
						std::bind(&trade_document_recognizer::preprocess_good_description,
						std::placeholders::_1),
						default_extract,
						std::bind(&trade_document_recognizer::postprocess_goods_description, std::placeholders::_1, configuration));

					if (good_descriptions.empty()) {
						good_descriptions = extract_field_values(fields.at(L"GOODS DESCRIPTION"), blocks,
							std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 5, false),
							std::bind(&trade_document_recognizer::preprocess_good_description,
							std::placeholders::_1),
							default_extract,
							std::bind(&trade_document_recognizer::postprocess_goods_description, std::placeholders::_1, configuration));
					}
				}
				else if (category == L"COMMERCIAL INVOICE" || category == L"PACKING LIST") {
					good_descriptions = extract_field_values(fields.at(L"GOODS DESCRIPTION"), blocks,
						std::bind(find_down_lines, std::placeholders::_1, std::placeholders::_2, 0.25, 0.0, 12, false),
						std::bind(&trade_document_recognizer::preprocess_good_description,
						std::placeholders::_1),
						default_extract,
						std::bind(&trade_document_recognizer::postprocess_goods_description, std::placeholders::_1, configuration));
				}
				else if (category == L"LETTER OF CREDIT") {
					good_descriptions = extract_field_values(fields.at(L"GOODS DESCRIPTION"), blocks,
						std::bind(find_down_lines, std::placeholders::_1, std::placeholders::_2, 0.25, 0.0, 5, false),
						std::bind(&trade_document_recognizer::preprocess_good_description,
						std::placeholders::_1),
						default_extract,
						std::bind(&trade_document_recognizer::postprocess_goods_description, std::placeholders::_1, configuration));
				}
				else if (category == L"FIRM OFFER") {
					good_descriptions = extract_field_values(fields.at(L"GOODS DESCRIPTION"), blocks,
						std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 2.5, false),
						std::bind(&trade_document_recognizer::preprocess_good_description,
						std::placeholders::_1),
						default_extract,
						std::bind(&trade_document_recognizer::postprocess_goods_description, std::placeholders::_1, configuration));

					if (good_descriptions.empty()) {
						good_descriptions = extract_field_values(fields.at(L"GOODS DESCRIPTION"), blocks,
							std::bind(find_nearest_down_lines, std::placeholders::_1, std::placeholders::_2, 0.1, 0.0, 10, false, false, false),
							std::bind(&trade_document_recognizer::preprocess_good_description,
							std::placeholders::_1),
							default_extract,
							std::bind(&trade_document_recognizer::postprocess_goods_description, std::placeholders::_1, configuration));
					}
				}

				if (!good_descriptions.empty()) {
					const auto dictionary = get_dictionary_words(configuration, dictionaries_, L"goods");

					aho_corasick::wtrie trie;
					trie.case_insensitive().remove_overlaps().allow_space();
					build_trie(trie, dictionary);

					for (auto it = std::rbegin(good_descriptions); it != std::rend(good_descriptions); it++) {
						const auto text = std::get<1>(*it);
						const auto matches = trie.parse_text(text);

						if (!matches.empty())
							return text;
					}

					return std::get<1>(good_descriptions.back());
				}

				return L"";
			}

			static std::wstring
				extract_origin(const configuration& configuration, const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
				const std::vector<block>& blocks, const cv::Size& image_size)
			{
				if (fields.find(L"ORIGIN") == fields.end())
					return L"";

				std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>> filtered_fields;
				std::copy_if(std::cbegin(fields.at(L"ORIGIN")), std::cend(fields.at(L"ORIGIN")), std::back_inserter(filtered_fields), [&image_size](const std::tuple<cv::Rect, std::wstring, cv::Range>& origin) {
					return std::get<0>(origin).y > image_size.height / 15.;
				});

				std::vector<std::pair<cv::Rect, std::wstring>> origins;

				if (category == L"CERTIFICATE OF ORIGIN") {
					origins = extract_field_values(filtered_fields, blocks,
						std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.25, 1.0, 100, true),
						std::bind(&trade_document_recognizer::preprocess_country, std::placeholders::_1,
						configuration, L"countries"),
						default_extract,
						postprocess_uppercase);
					if (origins.empty()) {
						origins = extract_field_values(filtered_fields, blocks,
							std::bind(find_nearest_down_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false, false, false),
							std::bind(&trade_document_recognizer::preprocess_country, std::placeholders::_1,
							configuration, L"countries"),
							default_extract,
							postprocess_uppercase);
					}

					if (origins.empty()) {
						origins = extract_field_values(filtered_fields, blocks,
							search_self,
							std::bind(&trade_document_recognizer::preprocess_country, std::placeholders::_1,
							configuration, L"countries"),
							default_extract,
							postprocess_uppercase);
					}

					if (origins.empty()) {
						origins = extract_field_values(filtered_fields, blocks,
							std::bind(find_nearest_down_lines, std::placeholders::_1, std::placeholders::_2, 0.35, 0.5, 7, false, false, false),
							std::bind(&trade_document_recognizer::preprocess_country, std::placeholders::_1,
							configuration, L"countries"),
							default_extract,
							postprocess_uppercase);
					}
				}
				else if (category == L"COMMERCIAL INVOICE" || category == L"PACKING LIST" || category == L"INSURANCE POLICY" || category == L"CERTIFICATE" || category == L"BILL OF LADING") {
					origins = extract_field_values(filtered_fields, blocks,
						search_self,
						std::bind(&trade_document_recognizer::preprocess_country, std::placeholders::_1,
						configuration, L"countries"),
						default_extract,
						postprocess_uppercase);
				}
				else if (category == L"FIRM OFFER") {
					origins = extract_field_values(filtered_fields, blocks,
						std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.25, 1.0, 100, true),
						std::bind(&trade_document_recognizer::preprocess_country, std::placeholders::_1,
						configuration, L"countries"),
						default_extract,
						postprocess_uppercase);
					if (origins.empty()) {
						origins = extract_field_values(filtered_fields, blocks,
							std::bind(find_nearest_down_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false, false, false),
							std::bind(&trade_document_recognizer::preprocess_country, std::placeholders::_1,
							configuration, L"countries"),
							default_extract,
							postprocess_uppercase);
					}
				}

				if (!origins.empty()) {
					return create_postprocess_function(configuration, L"country-countrycode.csv")(std::get<1>(origins[0]));
				}

				return L"";
			}

			static std::vector<std::wstring>
				extract_company_and_address(const configuration& configuration, const std::wstring& category, const std::wstring& basis,
				const std::wstring& below,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>&
				fields,
				const std::vector<block>& blocks)
			{
				if (fields.find(basis) == fields.end() || fields.find(below) == fields.end())
					return std::vector<std::wstring>();

				for (const auto& basis_field : fields.at(basis)) {
					const auto suspected_below_fields = find_nearest_down_lines(basis_field, blocks);
					if (suspected_below_fields.empty())
						continue;

					for (const auto& below_fields : fields.at(below)) {
						if (to_rect(suspected_below_fields) == to_rect(below_fields)) {
							auto suspected_field_values = find_right_lines(basis_field, blocks);
							if (suspected_field_values.size() >= 1) {
								if (suspected_field_values.size() > 1) {
									auto basis_rect = to_rect(basis_field);
									boost::remove_erase_if(suspected_field_values, [&basis_rect](const std::pair<cv::Rect, std::wstring>& field) {
										auto rect = to_rect(field);
										return rect.height > basis_rect.height * 1.2;
									});
								}
								auto right_field = std::make_tuple(std::get<0>(suspected_field_values[0]), std::get<1>(suspected_field_values[0]), cv::Range());

								auto temp_suspected_field_values = find_down_lines(right_field, blocks);
								for (const auto& temp : temp_suspected_field_values) {
									suspected_field_values.emplace_back(temp);
								}
							}

							std::sort(std::begin(suspected_field_values), std::end(suspected_field_values),
								[](const std::pair<cv::Rect, std::wstring>& a, const std::pair<cv::Rect, std::wstring>& b) {
								return to_rect(a).y < to_rect(b).y;
							});

							std::transform(std::begin(suspected_field_values), std::end(suspected_field_values),
								std::begin(suspected_field_values), [](const std::pair<cv::Rect, std::wstring>& field) {
								return std::make_pair(std::get<0>(field),
									boost::regex_replace(std::get<1>(field),
									boost::wregex(L"[^\\.,a-zA-Z0-9-]"),
									L""));
							});

							boost::remove_erase_if(suspected_field_values,
								[&basis_field, &below_fields](const std::pair<cv::Rect, std::wstring>& field) {
								const auto rect = to_rect(field);
								const auto top = rect.br().y > to_rect(basis_field).y;
								const auto bottom = rect.br().y < to_rect(below_fields).y;
								auto text = to_wstring(field);
								return !(top && bottom) || text.empty();
							});

							if (suspected_field_values.size() > 1) {
								auto second_line = to_wstring(suspected_field_values[1]);
								std::vector<std::wstring> words;
								boost::algorithm::split(words, second_line, boost::is_any_of(L" "), boost::token_compress_on);

								const auto keywords = get_dictionary_words(configuration, dictionaries_, L"companies");
								const auto dictionary = build_spell_dictionary(configuration, L"companies");
								aho_corasick::wtrie trie;
								build_trie(trie, keywords);

								if (words.size() > 1) {
									for (auto& word : words) {
										const auto suggested = dictionary->Correct(word);
										if (!suggested.empty() && suggested[0].distance <= get_distance_threshold(word))
											word = suggested[0].term;
									};

									second_line = boost::algorithm::join(words, L" ");
								}

								const auto matches = trie.parse_text(second_line);
								if (!matches.empty()) {
									for (const auto& match : matches) {
										if (match.get_end() + 1 == second_line.size() ||
											second_line[match.get_end() + 1] == L'.') {
											std::get<0>(suspected_field_values[0]) |= std::get<0>(suspected_field_values[1]);
											std::get<1>(suspected_field_values[0]) += L" " + second_line;
											suspected_field_values.erase(std::begin(suspected_field_values) + 1);
											break;
										}
									}
								}

								std::vector<std::wstring> basis_fields;
								for (auto i = 1; i < suspected_field_values.size(); i++) {
									auto r1 = to_rect(suspected_field_values[0]);
									auto r2 = to_rect(suspected_field_values[i]);
									if (r2.x > r1.br().x) {
										suspected_field_values.erase(suspected_field_values.begin() + i);
										i--;
									}
								}
								std::transform(std::begin(suspected_field_values), std::end(suspected_field_values),
									std::back_inserter(basis_fields), [](const std::pair<cv::Rect, std::wstring>& field) {
									return postprocess_uppercase(to_wstring(field));
								});

								return basis_fields;
							}
						}
					}
				}

				return std::vector<std::wstring>();
			}

			static std::vector<std::wstring>
				extract_bank_and_address(const configuration& configuration, const std::wstring& category, const std::wstring& basis,
				const std::wstring& below,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>&
				fields,
				const std::vector<block>& blocks)
			{
				if (fields.find(basis) == fields.end() || fields.find(below) == fields.end())
					return std::vector<std::wstring>();

				for (const auto& basis_field : fields.at(basis)) {
					auto basis_range = std::get<2>(basis_field);
					if (basis_range.start > 5) {
						continue;
					}
					auto basis_bank = to_wstring(basis_field, true);
					auto first_line = boost::regex_replace(basis_bank, boost::wregex(L": "), L"");
					auto suspected_swift_code = boost::regex_replace(basis_bank, boost::wregex(L"[^a-zA-Z0-9]"), L"");

					auto suspected_below_fields = find_down_lines(basis_field, blocks, 0.2);

					for (const auto& below_fields : fields.at(below)) {
						std::vector<std::wstring> bank_and_address;

						if (suspected_swift_code.size() >= 5) {
							bank_and_address.emplace_back(first_line);
							if (to_rect(suspected_below_fields[0]) == to_rect(below_fields)) {
								suspected_below_fields = find_down_lines(basis_field, blocks, 0.25);
							}
						}
						else {
							auto basis_right_line = find_nearest_right_line(basis_field, blocks);
							suspected_swift_code = boost::regex_replace(to_wstring(basis_right_line, true), boost::wregex(L"^a-zA-Z0-9]"), L"");
							if (suspected_swift_code.size() >= 5) {
								auto first_line = boost::regex_replace(to_wstring(basis_right_line), boost::wregex(L": "), L"");
								bank_and_address.emplace_back(first_line);
								if (to_rect(suspected_below_fields[0]) == to_rect(below_fields)) {
									auto right_field = std::make_tuple(std::get<0>(basis_right_line[0]), std::get<1>(basis_right_line[0]), cv::Range());
									suspected_below_fields = find_down_lines(right_field, blocks);
								}
							}
						}

						if (bank_and_address.empty() && !suspected_below_fields.empty() && to_rect(suspected_below_fields[0]) == to_rect(below_fields)) {
							auto basis_right_line = find_nearest_right_line(basis_field, blocks);
							if (!basis_right_line.empty()) {
								auto right_field = std::make_tuple(std::get<0>(basis_right_line[0]), std::get<1>(basis_right_line[0]), cv::Range());
								suspected_below_fields = find_down_lines(right_field, blocks);
								if (!suspected_below_fields.empty()) {
									bank_and_address.emplace_back(to_wstring(basis_right_line));
								}
							}
						}


						for (auto i = 0; i < suspected_below_fields.size(); i++) {
							if (to_rect(suspected_below_fields[i]).br().y >= to_rect(below_fields).y) {
								break;
							}
							if (i > 0) {
								auto line_rect = to_rect(suspected_below_fields[i]);
								auto prev_line_rect = to_rect(suspected_below_fields[i - 1]);
								if (prev_line_rect.br().y + prev_line_rect.height * 1.2 < line_rect.y) {
									break;
								}
							}
							const auto field = preprocess_remove_phone_number(to_wstring(suspected_below_fields[i]));
							if (!field.empty())
								bank_and_address.emplace_back(field);
						}

						std::transform(std::begin(bank_and_address), std::end(bank_and_address),
							std::begin(bank_and_address), [](const std::wstring& field) {
							return postprocess_uppercase(field);
						});

						if (!bank_and_address.empty())
							return bank_and_address;
					}
					if (fields.at(below).empty()) {
						std::vector<std::wstring> bank_and_address;
						if (suspected_swift_code.size() >= 5) {
							bank_and_address.emplace_back(first_line);
							suspected_below_fields = find_nearest_down_lines(basis_field, blocks, 0.25, 0.0, 10, true);
						}
						else {
							auto basis_right_line = find_nearest_right_line(basis_field, blocks);
							suspected_swift_code = boost::regex_replace(to_wstring(basis_right_line, true), boost::wregex(L"[^a-zA-Z0-9]"), L"");
							if (suspected_swift_code.size() >= 5) {
								auto first_line = boost::regex_replace(to_wstring(basis_right_line), boost::wregex(L": "), L"");
								bank_and_address.emplace_back(first_line);
								auto right_field = std::make_tuple(std::get<0>(basis_right_line[0]), std::get<1>(basis_right_line[0]), cv::Range());
								suspected_below_fields = find_nearest_down_lines(right_field, blocks, 0.25, 0.0, 10, true);
							}
						}

						for (auto i = 0; i < suspected_below_fields.size(); i++) {
							const auto field = preprocess_remove_phone_number(to_wstring(suspected_below_fields[i]));
							if (!field.empty())
								bank_and_address.emplace_back(field);
						}

						std::transform(std::begin(bank_and_address), std::end(bank_and_address),
							std::begin(bank_and_address), [](const std::wstring& field) {
							return postprocess_uppercase(field);
						});

						if (!bank_and_address.empty())
							return bank_and_address;
					}
				}

				return std::vector<std::wstring>();
			}

			static std::vector<std::wstring>
				extract_company(const configuration& configuration, const std::wstring& category, const std::wstring& field,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>&
				fields,
				const std::vector<block>& blocks)
			{
				if (fields.find(field) == fields.end())
					return std::vector<std::wstring>();

				const auto company = extract_field_values(fields.at(field), blocks,
					std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
					std::bind(&trade_document_recognizer::preprocess_company, std::placeholders::_1,
					configuration, L"companies"),
					default_extract,
					postprocess_uppercase);

				if (company.empty()) {
					return std::vector<std::wstring>();
				}

				return std::vector<std::wstring>{ to_wstring(company[0]) };
			}

			static std::vector<std::wstring>
				extract_company_and_address(const configuration& configuration, const std::wstring& category, const std::wstring& field,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>&
				fields,
				const std::vector<block>& blocks)
			{
				std::vector<std::wstring> company_and_address;

				if (fields.find(field) == fields.end())
					return company_and_address;

				std::vector<std::pair<cv::Rect, std::wstring>> company;
				if (category == L"LETTER OF GUARANTEE") {
					if (field != L"NOTIFY") {
						company = extract_field_values(fields.at(field), blocks,
							std::bind(find_nearest_down_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 4, true, false, false),
							default_preprocess,
							default_extract,
							postprocess_uppercase);
					}
					else {
						company = extract_field_values(fields.at(field), blocks,
							std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.25, 1.0, 10, true),
							std::bind(&trade_document_recognizer::preprocess_company, std::placeholders::_1,
							configuration, L"companies"),
							default_extract,
							postprocess_uppercase);
					}

					if (!company.empty()) {
						company_and_address.emplace_back(postprocess_uppercase(to_wstring(company[0])));
						const auto address_blocks = find_nearest_down_lines(to_block(company[0]), blocks, 0.5, 0.0, 2, false);
						for (auto i = 0; i < address_blocks.size(); i++) {
							const auto address_block = address_blocks[i];
							company_and_address.emplace_back(postprocess_uppercase(to_wstring(address_block)));
							if (find_address_country(address_block, configuration, L"countries", i))
								break;
						}
					}

					for (auto i = 0; i < company_and_address.size(); i++) {
						if (boost::regex_search(company_and_address[i], boost::wregex(L"SHIPPER"))) {
							company_and_address.resize(i);
							break;
						}
					}

					return company_and_address;
				}

				if (company.empty() && field != L"ISSUING BANK") {
					company = extract_field_values(fields.at(field), blocks,
						std::bind(find_nearest_down_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false, false, false),
						std::bind(&trade_document_recognizer::preprocess_company, std::placeholders::_1,
						configuration, L"companies"),
						default_extract,
						postprocess_uppercase);
				}

				if (company.empty() && (category != L"BILL OF LADING" && category != L"CARGO RECEIPT")) {
					company = extract_field_values(fields.at(field), blocks,
						std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						std::bind(&trade_document_recognizer::preprocess_company, std::placeholders::_1,
						configuration, L"companies"),
						default_extract,
						postprocess_uppercase);
				}
				if (company.empty() && category == L"FIRM OFFER") {
					company = extract_field_values(fields.at(field), blocks,
						search_self,
						std::bind(&trade_document_recognizer::preprocess_company, std::placeholders::_1,
						configuration, L"companies"),
						default_extract,
						postprocess_uppercase);
				}

				if (company.empty()) {
					int max_basis_char_count = 0;
					for (auto& basis : fields.at(field)) {
						auto basis_str = to_wstring(basis);
						if (max_basis_char_count < basis_str.size()) {
							max_basis_char_count = basis_str.size();
						}
					}
					auto expanded_ratio = max_basis_char_count < 10 ? 1.0 : 0.2;
					company = extract_field_values(fields.at(field), blocks,
						std::bind(find_nearest_down_lines, std::placeholders::_1, std::placeholders::_2, 0.15, expanded_ratio, 100, true, false, false),
						default_preprocess,
						default_extract,
						postprocess_uppercase);
					if (!company.empty()) {
						bool is_close = false;
						for (auto& basis : fields.at(field)) {
							auto rect1 = to_rect(basis);
							auto rect2 = to_rect(company.front());
							if (rect1.br().y + rect1.height * 3.7 > rect2.y && rect1.y < rect2.y) {
								is_close = true;
								break;
							}
						}
						bool include_company = false;
						for (const auto& line : company) {
							auto processed = preprocess_company(line, configuration, L"companies");
							if (!to_wstring(processed).empty()) {
								include_company = true;
								break;
							}
						}
						if (!include_company) {
							auto company_name = extract_field_values(fields.at(field), blocks,
								search_self,
								std::bind(&trade_document_recognizer::preprocess_company, std::placeholders::_1,
								configuration, L"companies"),
								default_extract,
								postprocess_uppercase);
							if (company_name.empty() && category == L"FIRM OFFER" && field == L"COLLECTING BANK") {
								for (auto& f : fields.at(field)) {
									company_name = search_self(f, blocks);
									if (!boost::regex_search(to_wstring(company_name), boost::wregex(L"BANK", boost::regex::icase))) {
										company_name.clear();
									}
								}
							}
							if (!company_name.empty() && is_close) {
								company_and_address.emplace_back(to_wstring(company_name));
								include_company = true;
							}
						}

						if (is_close) {
							for (auto i = 0; i < company.size(); i++) {
								company_and_address.emplace_back(postprocess_uppercase(to_wstring(company[i])));
								if (find_address_country(company[i], configuration, L"countries", i))
									break;
							}
						}
						if (category == L"FIRM OFFER") {
							for (auto i = 0; i < company_and_address.size(); i++) {
								company_and_address[i] = boost::regex_replace(company_and_address[i], boost::wregex(L"^ADDRESS: ?", boost::regex::icase), L"");
								if (boost::regex_search(company_and_address[i], boost::wregex(L"PLEASED", boost::regex::icase))
									|| boost::regex_search(company_and_address[i], boost::wregex(L"PORT OF", boost::regex::icase))
									|| boost::regex_search(company_and_address[i], boost::wregex(L"PAYMENT", boost::regex::icase))
									|| boost::regex_search(company_and_address[i], boost::wregex(L"DATE", boost::regex::icase))) {
									company_and_address.resize(i);
									break;
								}
							}
						}
						else if (category == L"COMMERCIAL INVOICE" || category == L"PACKING LIST") {
							for (auto i = 0; i < company_and_address.size(); i++) {
								if (boost::regex_search(company_and_address[i], boost::wregex(L"SHIPPED PER", boost::wregex::icase))
									|| boost::regex_search(company_and_address[i], boost::wregex(L"ON OR ABOUT", boost::regex::icase))) {
									company_and_address.resize(i);
									break;
								}
							}
						}

					}
				}
				else {
					company_and_address.emplace_back(postprocess_uppercase(to_wstring(company[0])));
					const auto address_blocks = find_nearest_down_lines(to_block(company[0]), blocks, 0.5, 0.0, 10, true);
					for (auto i = 0; i < address_blocks.size(); i++) {
						const auto address_block = address_blocks[i];

						auto rect1 = i == 0 ? to_rect(company.front()) : to_rect(address_blocks[i - 1]);
						auto rect2 = to_rect(address_block);
						if (rect1.br().y + rect1.height * 2 < rect2.y) {
							break;
						}

						company_and_address.emplace_back(postprocess_uppercase(to_wstring(address_block)));
						if (find_address_country(address_block, configuration, L"countries", i))
							break;
					}
				}

				for (auto i = 0; i < company_and_address.size(); i++) {
					if (boost::regex_search(company_and_address[i], boost::wregex(L"(?:GOODS? ) ?DESCRIPTION"))) {
						company_and_address.resize(i);
						break;
					}
				}

				if (!company_and_address.empty()) {
					company_and_address[0] = boost::regex_replace(company_and_address[0], boost::wregex(L"^(?:TO )?THE ORDER OF:?", boost::regex::icase), L"");
					company_and_address[0] = boost::regex_replace(company_and_address[0], boost::wregex(L"^TO ORDER", boost::regex::icase), L"");
					company_and_address[0] = boost::regex_replace(company_and_address[0], boost::wregex(L"^TO ", boost::regex::icase), L"");
					company_and_address[0] = boost::regex_replace(company_and_address[0], boost::wregex(L"^: ?", boost::regex::icase), L"");
					company_and_address[0] = boost::trim_copy(company_and_address[0]);

					if (boost::iequals(company_and_address[0], L"SAID TO CONTAIN"))
						return std::vector<std::wstring>();
				}

				if (field == L"AGENT") {
					company_and_address.resize(1);
				}

				return company_and_address;
			}

			static std::vector<std::wstring>
				extract_issuing_bank(const configuration& configuration, const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
				const std::vector<block>& blocks, const cv::Size& image_size)
			{
				std::vector<std::wstring> bank_lines;
				if (fields.find(L"ISSUING BANK") == fields.end())
					return bank_lines;

				std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>> new_field;
				for (auto& field : fields.at(L"ISSUING BANK")) {
					auto basis_range = std::get<2>(field);
					if (basis_range.start > 3) {
						continue;
					}
					new_field.emplace_back(field);
				}

				auto bank = extract_field_values(new_field, blocks,
					std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
					std::bind(&trade_document_recognizer::preprocess_company, std::placeholders::_1,
					configuration, L"companies"),
					default_extract,
					postprocess_uppercase);
				if (bank.empty()) {
					bank = extract_field_values(new_field, blocks,
						search_self,
						std::bind(&trade_document_recognizer::preprocess_company, std::placeholders::_1,
						configuration, L"companies"),
						default_extract,
						postprocess_uppercase);
				}
				if (bank.empty()) {
					bank = extract_field_values(new_field, blocks,
						std::bind(find_nearest_down_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 3, false, false, false),
						std::bind(&trade_document_recognizer::preprocess_company, std::placeholders::_1,
						configuration, L"companies"),
						default_extract,
						postprocess_uppercase);
				}
				if (bank.empty()) {
					bank = extract_field_values(new_field, blocks,
						std::bind(find_nearest_down_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, true, false, false),
						default_preprocess,
						default_extract,
						postprocess_uppercase);
					if (!bank.empty()) {
						bool is_close = false;
						for (auto& basis : new_field) {
							auto rect1 = to_rect(basis);
							auto rect2 = to_rect(bank.front());
							if (rect1.br().y + rect1.height*1.5 > rect2.y && rect1.y < rect2.y) {
								is_close = true;
								break;
							}
						}
						bool include_company = false;
						for (const auto& line : bank) {
							auto processed = preprocess_company(line, configuration, L"companies");
							if (!to_wstring(processed).empty()) {
								include_company = true;
								break;
							}
						}
						if (!include_company) {
							auto bank_name = extract_field_values(new_field, blocks,
								search_self,
								std::bind(&trade_document_recognizer::preprocess_company, std::placeholders::_1,
								configuration, L"companies"),
								default_extract,
								postprocess_uppercase);
							if (!bank_name.empty() && is_close) {
								bank_lines.emplace_back(to_wstring(bank_name));
								include_company = true;
							}
						}

						if (is_close) {
							for (const auto& line : bank) {
								bank_lines.emplace_back(postprocess_uppercase(to_wstring(line)));
								if (find_company(line, configuration, L"companies"))
									break;
							}
						}
					}
				}
				else {
					bank_lines.emplace_back(postprocess_uppercase(to_wstring(bank[0])));
					if (!find_company(bank[0], configuration, L"companies")) {
						const auto address_blocks = find_nearest_down_lines(to_block(bank[0]), blocks, 0.5, 0.0, 10, true);
						for (const auto& address_block : address_blocks) {
							bank_lines.emplace_back(postprocess_uppercase(to_wstring(address_block)));
							if (find_company(address_block, configuration, L"companies"))
								break;
						}
					}
				}

				return bank_lines;
			}

			static std::unordered_map<std::wstring, std::vector<std::wstring>>
				extract_banks(const configuration& configuration, const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
				const std::vector<block>& blocks, const cv::Size& image_size)
			{
				if (category == L"BILL OF EXCHANGE") {
					if (fields.find(L"BANK") == fields.end())
						return std::unordered_map<std::wstring, std::vector<std::wstring>>();

					auto banks = extract_field_values(fields.at(L"BANK"), blocks,
						search_self,
						default_preprocess,
						create_extract_function(L"(?:PAY TO )?(?:THE ORDER OF )?(?:(.*BANK.*(?:BRANCH)?)(?:\\s+(?:(?:OF|OR) ORDER)\\s+)|(.*BANK.*(?:BRANCH)?))"),
						postprocess_uppercase);
					if (banks.empty())
						banks = extract_field_values(fields.at(L"BANK"), blocks,
						std::bind(find_right_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
						default_preprocess,
						create_extract_function(L"(?:PAY TO )?(?:THE ORDER OF )?(?:(.*BANK.*(?:BRANCH)?)(?:\\s+(?:(?:OF|OR) ORDER\\s+)|(.*BANK.*(?:BRANCH)?))"),
						postprocess_uppercase);
					if (banks.empty())
						return std::unordered_map<std::wstring, std::vector<std::wstring>>();

					const auto bank_name = boost::regex_replace(to_wstring(banks[0]), boost::wregex(L".*(?:PAY TO|ORDER OF|(?:OF|OR) ORDER)\\s+", boost::regex_constants::icase), L"");
					return std::unordered_map<std::wstring, std::vector<std::wstring>>{
						std::make_pair(L"NEGOTIATION BANK", std::vector<std::wstring>{bank_name})
					};
				}
				else if (category == L"LETTER OF CREDIT") {
					std::unordered_map<std::wstring, std::vector<std::wstring>> banks;
					auto bank = extract_bank_and_address(configuration, category, L"SWIFT ISSUING BANK", L"SWIFT COLLECTING BANK", fields,
						blocks);
					if (!bank.empty())
						banks.emplace(std::make_pair(L"ISSUING BANK", bank));
					if (fields.find(L"SWIFT COLLECTING BANK") != fields.end()) {
						std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>> new_fields;
						for (auto& field : fields.at(L"SWIFT COLLECTING BANK")) {
							auto str = to_wstring(field);
							if (!boost::regex_search(str, boost::wregex(L"전자서명", boost::regex::icase))) {
								std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>> new_field;
								new_field.emplace_back(field);
								new_fields.insert(std::make_pair(L"SWIFT COLLECTING BANK", new_field));
								break;
							}
						}
						for (auto& field : fields.at(L"ETC INFO")) {
							std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>> new_field;
							new_field.emplace_back(field);
							new_fields.insert(std::make_pair(L"ETC INFO", new_field));
						}
						bank = extract_bank_and_address(configuration, category, L"SWIFT COLLECTING BANK", L"ETC INFO", new_fields, blocks);
					}

					if (!bank.empty())
						banks.emplace(std::make_pair(L"COLLECTING BANK", bank));
					if (fields.find(L"ISSUING BANK") != fields.end()) {
						std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>> new_fields;
						for (auto& field : fields.at(L"ISSUING BANK")) {
							auto str = to_wstring(field);
							if (!boost::regex_search(str, boost::wregex(L"BY AUTHENTICATED", boost::regex::icase))) {
								std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>> new_field;
								new_field.emplace_back(field);
								new_fields.insert(std::make_pair(L"ISSUING BANK", new_field));
								break;
							}
						}
						bank = extract_bank_and_address(configuration, category, L"ISSUING BANK", L"APPLICANT", new_fields, blocks);
					}

					if (!bank.empty())
						banks.emplace(std::make_pair(L"ISSUING BANK", bank));

					return banks;
				}
				else if (category == L"SHIPMENT ADVICE") {
					std::unordered_map<std::wstring, std::vector<std::wstring>> banks;
					auto bank = extract_company_and_address(configuration, category, L"ISSUING BANK", fields, blocks);
					if (!bank.empty())
						banks.emplace(std::make_pair(L"ISSUING BANK", bank));

					return banks;
				}
				else if (category == L"FIRM OFFER") {
					std::unordered_map<std::wstring, std::vector<std::wstring>> banks;
					auto bank = extract_company_and_address(configuration, category, L"COLLECTING BANK", fields, blocks);
					if (!bank.empty())
						banks.emplace(std::make_pair(L"COLLECTING BANK", bank));

					return banks;
				}

				return std::unordered_map<std::wstring, std::vector<std::wstring>>();
			}

			static std::vector<std::wstring>
				extract_applicant(const configuration& configuration, const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
				const std::vector<block>& blocks, const cv::Size& image_size)
			{
				if (category == L"LETTER OF CREDIT")
					return extract_company_and_address(configuration, category, L"APPLICANT", L"BENEFICIARY", fields, blocks);
				if (category == L"COMMERCIAL INVOICE" || category == L"PACKING LIST" || category == L"SHIPMENT ADVICE" || category == L"FIRM OFFER")
					return extract_company_and_address(configuration, category, L"APPLICANT", fields, blocks);
				return std::vector<std::wstring>();
			}

			static std::vector<std::wstring>
				extract_beneficiary(const configuration& configuration, const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
				const std::vector<block>& blocks, const cv::Size& image_size)
			{
				if (category == L"LETTER OF CREDIT")
					return extract_company_and_address(configuration, category, L"BENEFICIARY", L"AMOUNT", fields, blocks);
				if (category == L"COMMERCIAL INVOICE" || category == L"PACKING LIST" || category == L"SHIPMENT ADVICE")
					return extract_company_and_address(configuration, category, L"BENEFICIARY", fields, blocks);
				return std::vector<std::wstring>();
			}

			static std::vector<std::wstring>
				extract_shipping_line(const configuration& configuration, const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
				const std::vector<block>& blocks, const cv::Size& image_size)
			{
				if (category == L"SHIPMENT ADVICE") {
					return extract_company(configuration, category, L"SHIPPING LINE", fields, blocks);
				}
				else if (category == L"BILL OF LADING" || category == L"CARGO RECEIPT") {
					const auto paper = image_size;

					if (blocks.empty())
						return std::vector<std::wstring>();

					std::vector<line> right_lines;
					std::copy_if(std::begin(blocks[0]), std::end(blocks[0]), std::back_inserter(right_lines), [&](const line& line) {
						const auto line_rect = to_rect(line);
						return paper.width / 2 < line_rect.x && paper.height / 4 > line_rect.y;
					});

					auto companies = extract_field_values(std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>{
						std::make_tuple(cv::Rect(), L"", cv::Range())
					}, std::vector<block> {right_lines},
					default_search,
					std::bind(&trade_document_recognizer::preprocess_company, std::placeholders::_1,
					configuration, L"companies"),
					default_extract,
					postprocess_uppercase);

					boost::remove_erase_if(companies, [](const std::pair<cv::Rect, std::wstring>& company) {
						const auto company_name = to_wstring(company);
						return company_name.find(L"BILL OF") != std::wstring::npos;
					});

					if (companies.empty()) {
						std::vector<line> left_lines;
						std::copy_if(std::begin(blocks[0]), std::end(blocks[0]), std::back_inserter(left_lines), [&](const line& line) {
							const auto line_rect = to_rect(line);
							return paper.width / 2 > line_rect.x && paper.height * 0.07 > line_rect.y;
						});

						companies = extract_field_values(std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>{
							std::make_tuple(cv::Rect(), L"", cv::Range())
						}, std::vector<block> {left_lines},
						default_search,
						std::bind(&trade_document_recognizer::preprocess_company, std::placeholders::_1,
						configuration, L"companies"),
						default_extract,
						postprocess_uppercase);

						boost::remove_erase_if(companies, [](const std::pair<cv::Rect, std::wstring>& company) {
							const auto company_name = to_wstring(company);
							return boost::regex_search(company_name, boost::wregex(L"(?:BILL OF|IF ANY|RECEIVED [BD]Y|THE CARRIER|THE GOODS AS)\\s+", boost::regex_constants::icase));
						});
					}

					if (companies.empty())
						return std::vector<std::wstring>();

					const auto shipping_line = std::max_element(std::begin(companies), std::end(companies),
						[](const std::pair<cv::Rect, std::wstring>& a,
						const std::pair<cv::Rect, std::wstring>& b) {
						return to_rect(a).height < to_rect(b).height;
					});

					const auto shipping_line_name = to_wstring(*shipping_line);
					return std::vector<std::wstring>{shipping_line_name};
				}

				return std::vector<std::wstring>();
			}

			static std::vector<std::wstring>
				extract_seller(const configuration& configuration, const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
				const std::vector<block>& blocks, const cv::Size& image_size)
			{
				if (category == L"COMMERCIAL INVOICE") {
					std::vector<std::wstring> extracted_result;

					std::vector<std::pair<cv::Rect, std::wstring>> result;

					result = extract_field_values(fields.at(L"SELLER"), blocks,
						std::bind(find_nearest_down_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false, false, false),
						//std::bind(&trade_document_recognizer::preprocess_port_of_discharge, std::placeholders::_1, configuration, L"PORT OF DISCHARGE"),
						//default_preprocess,
						preprocess_port_of_discharge,
						default_extract,
						postprocess_discharge);

					if (result.empty()) {
						result = extract_field_values(fields.at(L"SELLER"), blocks,
							std::bind(find_down_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 100, false),
							preprocess_port_of_discharge,
							default_extract,
							postprocess_discharge);
						//					return extracted_result;
					}
					else if (to_wstring(result[0]).length() > 50) {
						return extracted_result;
					}
					else {
						//for (auto& a : result) {
						extracted_result.emplace_back(to_wstring(result[0]));
						//}
					}

					if (result.empty()) {
						return extracted_result;
					}

					return extracted_result;
				}
				
			}

			static std::vector<std::wstring>
				extract_buyer(const configuration& configuration, const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
				const std::vector<block>& blocks, const cv::Size& image_size)
			{
				if (category == L"COMMERCIAL INVOICE" || category == L"PACKING LIST") {
					std::vector<std::wstring> company_and_addresses;

					if (fields.at(L"BUYER").size() > 0) {
						const auto paper = image_size;
						std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>> up_side_field;
						bool contain_sold_to = false;
						for (auto& field : fields.at(L"BUYER")) {
							auto str = to_wstring(field, false);
							if (boost::regex_search(str, boost::wregex(L"sold to", boost::regex::icase))) {
								contain_sold_to = true;
								std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>> new_field;
								new_field.emplace_back(field);
								up_side_field.insert(std::make_pair(L"BUYER", new_field));
								break;
							}
						}
						if (!contain_sold_to) {
							for (auto& field : fields.at(L"BUYER")) {
								auto rect = to_rect(field);
								auto str = to_wstring(field, true);
								if (boost::regex_search(str, boost::wregex(L"order no", boost::regex::icase))) {
									continue;
								}
								if (rect.y < paper.height * 2 / 5) {
									std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>> new_field;
									new_field.emplace_back(field);
									up_side_field.insert(std::make_pair(L"BUYER", new_field));
									break;
								}
							}
						}


						if (up_side_field.find(L"BUYER") == up_side_field.end())
							return std::vector<std::wstring>();


						company_and_addresses = extract_company_and_address(configuration, category, L"BUYER", up_side_field, blocks);
					}

					if (company_and_addresses.empty())
						company_and_addresses = extract_company_and_address(configuration, category, L"BUYER", fields, blocks);

					if (!company_and_addresses.empty()) {
						if (std::any_of(std::begin(company_and_addresses), std::end(company_and_addresses), [](const std::wstring& address) {
							if (address.find(L"ITEM") == 0)
								return true;
							return false;
						}))
							return std::vector<std::wstring>();

						if (company_and_addresses.size() > 4) {
							company_and_addresses[3] += L" " + company_and_addresses[4];
							company_and_addresses.erase(company_and_addresses.begin() + 4);
						}

						return company_and_addresses;
					}
				}
				else if (category == L"FIRM OFFER") {
					return extract_company_and_address(configuration, category, L"BUYER", fields, blocks);
				}

				return std::vector<std::wstring>();
			}

			static std::vector<std::wstring>
				extract_importer(const configuration& configuration, const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
				const std::vector<block>& blocks, const cv::Size& image_size)
			{
				if (category == L"CERTIFICATE OF ORIGIN")
					return extract_company_and_address(configuration, category, L"IMPORTER", fields, blocks);
				else if (category == L"COMMERCIAL INVOICE" || category == L"PACKING LIST")
					return extract_company_and_address(configuration, category, L"IMPORTER", fields, blocks);
				return std::vector<std::wstring>();
			}

			static std::vector<std::wstring>
				extract_drawer(const configuration& configuration, const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
				const std::vector<block>& blocks, const cv::Size& image_size)
			{
				if (category == L"REMITTANCE LETTER") {
					return extract_company_and_address(configuration, category, L"DRAWER", fields, blocks);
				}

				return std::vector<std::wstring>();
			}

			static std::vector<std::wstring>
				extract_exporter(const configuration& configuration, const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
				const std::vector<block>& blocks, const cv::Size& image_size)
			{

				if (fields.at(L"EXPORTER").size() > 0) {
					const auto paper = image_size;
					std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>> up_side_field;
					for (auto& field : fields.at(L"EXPORTER")) {
						auto rect = to_rect(field);
						if (rect.y < paper.height * 2 / 5) {
							std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>> new_field;
							new_field.emplace_back(field);
							up_side_field.insert(std::make_pair(L"EXPORTER", new_field));
							break;
						}
					}
					if (up_side_field.find(L"EXPORTER") == up_side_field.end())
						return std::vector<std::wstring>();

					return extract_company_and_address(configuration, category, L"EXPORTER", up_side_field, blocks);
				}
				return extract_company_and_address(configuration, category, L"EXPORTER", fields, blocks);
			}

			static std::wstring
				extract_exporter_ex(const configuration& configuration, const std::wstring& category,
					const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
					const std::vector<block>& blocks, const cv::Size& image_size)
			{
				if (fields.find(L"EXPORTER") == fields.end())
					return L"";
				//spdlog::get("recognizer")->info("{} : {} : [{}],[{}]", __FUNCTION__, __LINE__, fields.at(L"L/C NO"), blocks);

				std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>> filtered_fields;
				std::copy_if(std::begin(fields.at(L"EXPORTER")), std::end(fields.at(L"EXPORTER")), std::back_inserter(filtered_fields), [](const std::tuple<cv::Rect, std::wstring, cv::Range>& field) {
					const auto text = std::get<1>(field);
					if (boost::regex_search(text, boost::wregex(L"^to|^from", boost::regex::icase)))
						return true;
					if (boost::regex_search(text, boost::wregex(L"\\s+to:", boost::regex::icase)))
						return false;
					return true;
				});


				std::vector<std::pair<cv::Rect, std::wstring>> ret;

				ret = extract_field_values(fields.at(L"EXPORTER"), blocks,
					std::bind(find_nearest_down_lines, std::placeholders::_1, std::placeholders::_2, 0.01, 0.25, 2, false, false, false),
					default_preprocess,
					default_extract,
					default_postprocess);

				if (ret.empty()) {
					ret = extract_field_values(filtered_fields, blocks,
						std::bind(find_down_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 8, true),
						default_preprocess,
						default_extract,
						default_postprocess);

					if (ret.empty()) {
						ret = extract_field_values(filtered_fields, blocks,
							search_self,
							default_preprocess,
							default_extract,
							default_postprocess);
					}
				}
				/*
				boost::remove_erase_if(lcno, [](const std::pair<cv::Rect, std::wstring>& vessel) {
				return boost::regex_search(std::get<1>(vessel), boost::wregex(L"TO BE (?:DECLARED|ADVISED)", boost::regex_constants::icase)); });
				*/
				if (ret.empty())
					return L"";

				return std::get<1>(ret[0]);
			}


			static std::vector<std::wstring>
				extract_manufacturer(const configuration& configuration, const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
				const std::vector<block>& blocks, const cv::Size& image_size)
			{
				return extract_company_and_address(configuration, category, L"MANUFACTURER", fields, blocks);
			}

			static std::wstring
				extract_carrier(const configuration& configuration, const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
				const std::vector<block>& blocks, const cv::Size& image_size)
			{
				const auto paper = image_size;

				if (blocks.empty())
					return L"";

				std::vector<line> right_lines;
				std::copy_if(std::begin(blocks[0]), std::end(blocks[0]), std::back_inserter(right_lines), [&](const line& line) {
					const auto line_rect = to_rect(line);
					return paper.width / 2 < line_rect.x && paper.height / 4 * 3 < line_rect.y;
				});

				auto companies = extract_field_values(std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>{
					std::make_tuple(cv::Rect(), L"", cv::Range())
				}, std::vector<block> {right_lines}, default_search, std::bind(&trade_document_recognizer::preprocess_company, std::placeholders::_1, configuration, L"companies"), default_extract, postprocess_uppercase);

				boost::remove_erase_if(companies, [](const std::pair<cv::Rect, std::wstring>& company) {
					const auto company_name = to_wstring(company);
					return company_name.find(L"BILL OF") != std::wstring::npos;
				});

				if (companies.empty())
					return L"";

				std::wstring carrier_name;
				for (const auto& company : companies) {
					if (to_wstring(company).find(L"LINE A/S") != std::wstring::npos ||
						to_wstring(company).find(L"LINE") != std::wstring::npos)
						carrier_name = to_wstring(company);
				}

				if (carrier_name.empty()) {
					const auto carrier = std::max_element(std::begin(companies), std::end(companies),
						[](const std::pair<cv::Rect, std::wstring>& a,
						const std::pair<cv::Rect, std::wstring>& b) {
						return to_rect(a).height < to_rect(b).height;
					});

					carrier_name = to_wstring(*carrier);
				}

				return boost::trim_copy(boost::regex_replace(carrier_name, boost::wregex(L"(?:.* (?:(?:FOR|OF) THE (?:CARRIER|GAMER)|BEHALF OF) |\\s*(?:BY|ONBOARD|FOR|ISSUED|SIGNED)\\s|\\s*AS CARRIER[.,]?)", boost::regex::icase), L""));
			}

			static std::wstring
				extract_place_of_issue(const configuration& configuration, const std::wstring& category,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
				const std::vector<block>& blocks, const cv::Size& image_size)
			{
				if (fields.find(category) == fields.end())
					return L"";
				if (blocks.empty())
					return L"";

				auto country = extract_field_values(fields.at(L"PLACE OF ISSUE"), blocks,
					std::bind(find_nearest_right_line, std::placeholders::_1, std::placeholders::_2, 0.25, 0.0, 100, false),
					std::bind(&trade_document_recognizer::preprocess_country, std::placeholders::_1,
					configuration, L"countries"),
					default_extract,
					postprocess_uppercase);
				if (country.empty()) {
					country = extract_field_values(fields.at(L"PLACE OF ISSUE"), blocks,
						search_self,
						std::bind(&trade_document_recognizer::preprocess_country, std::placeholders::_1,
						configuration, L"countries"),
						default_extract,
						postprocess_uppercase);
				}
				if (country.empty()) {
					country = extract_field_values(fields.at(L"PLACE OF ISSUE"), blocks,
						std::bind(find_nearest_down_lines, std::placeholders::_1, std::placeholders::_2, 0.25, 0.0, 100, false, false, false),
						std::bind(&trade_document_recognizer::preprocess_country, std::placeholders::_1,
						configuration, L"countries"),
						default_extract,
						postprocess_uppercase);
				}

				if (country.empty()) {
					const auto paper = image_size;

					std::vector<line> bottom_lines;
					std::copy_if(std::begin(blocks[0]), std::end(blocks[0]), std::back_inserter(bottom_lines), [&](const line& line) {
						const auto line_rect = to_rect(line);
						return line_rect.y > paper.height * 3 / 4 && line.size() < 5;
					});

					country = extract_field_values(std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>{
						std::make_tuple(cv::Rect(), L"", cv::Range())
					}, std::vector<block> {bottom_lines},
					default_search,
					std::bind(&trade_document_recognizer::preprocess_country, std::placeholders::_1,
					configuration, L"countries"),
					default_extract,
					postprocess_uppercase);

				}

				if (country.empty())
					return L"";

				return to_wstring(country[0]);
			}

			static std::pair<std::wstring, std::wstring>
				extract_insurance_company(const configuration& configuration,
				const std::unordered_map<std::wstring, std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>>& fields,
				const std::vector<block>& blocks, const cv::Size& image_size, const std::wstring& keyword)
			{
				if (fields.find(keyword) == fields.end())
					return std::make_pair(L"", L"");

				std::wstring search_field = keyword;

				std::vector<std::pair<cv::Rect, std::wstring>> insurance_company;
				if (search_field != L"INSURANCE COMPANY") {
					insurance_company = extract_field_values(fields.at(search_field), blocks,
						search_self,
						std::bind(&trade_document_recognizer::preprocess_insurance_company, std::placeholders::_1, configuration, L"insurance_companies"),
						default_extract,
						postprocess_uppercase);

					if (!insurance_company.empty())
						return std::make_pair(std::get<1>(insurance_company[0]), L"");

					insurance_company = extract_field_values(fields.at(search_field), blocks,
						std::bind(find_nearest_down_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 0.0, 1, false, false, false),
						std::bind(&trade_document_recognizer::preprocess_insurance_company, std::placeholders::_1, configuration, L"insurance_companies"),
						default_extract,
						postprocess_uppercase);

					if (!insurance_company.empty()) {
						const auto country_code = extract_country_code(configuration, insurance_company[0], blocks, image_size);
						return std::make_pair(std::get<1>(insurance_company[0]), country_code);
					}

					insurance_company = extract_field_values(fields.at(search_field), blocks,
						std::bind(find_right_lines, std::placeholders::_1, std::placeholders::_2, 0.5, 4, 2, true),
						std::bind(&trade_document_recognizer::preprocess_insurance_company, std::placeholders::_1, configuration, L"insurance_companies"),
						default_extract,
						postprocess_uppercase);

					if (!insurance_company.empty())
						return std::make_pair(std::get<1>(insurance_company[0]), L"");
				}

				return std::make_pair(L"", L"");
			}

			static std::wstring
				extract_insurance_company(const configuration& configuration, const std::vector<block>& blocks, const cv::Size& image_size)
			{
				const auto paper = image_size;

				std::vector<line> upper_lines;
				std::copy_if(std::begin(blocks[0]), std::end(blocks[0]), std::back_inserter(upper_lines), [&](const line& line) {
					const auto line_rect = to_rect(line);
					return paper.height / 5 > line_rect.y;
				});

				auto companies = extract_field_values(std::vector<std::tuple<cv::Rect, std::wstring, cv::Range>>{
					std::make_tuple(cv::Rect(), L"", cv::Range())
				}, std::vector<block> { upper_lines },
				default_search,
				std::bind(&trade_document_recognizer::preprocess_insurance_company, std::placeholders::_1,
				configuration, L"insurance_companies"),
				default_extract,
				postprocess_uppercase);

				if (companies.empty())
					return L"";

				boost::remove_erase_if(companies, [](const std::pair<cv::Rect, std::wstring>& company) {
					const static std::vector<std::wstring> COMPANY_KEYWORDS{ L"INSURANCE", L"AIG", L"IAG", L"MARSH", L"SOLUTION", L"RISK", L"ADJUSTERS" };
					const auto company_name = std::get<1>(company);
					if (std::any_of(std::begin(COMPANY_KEYWORDS), std::end(COMPANY_KEYWORDS), [&company_name](const std::wstring& keyword) {
						return company_name.find(keyword) != std::wstring::npos;
					}))
						return false;
					return true;
				});

				if (companies.empty())
					return L"";

				const auto insurance_company = std::max_element(std::begin(companies), std::end(companies),
					[](const std::pair<cv::Rect, std::wstring>& a,
					const std::pair<cv::Rect, std::wstring>& b) {
					return to_rect(a).y > to_rect(b).y;
				});

				return to_wstring(*insurance_company);
			}

			static std::wstring extract_country_code(const configuration& configuration, const std::pair<cv::Rect, std::wstring>& basis, const std::vector<block>& blocks, const cv::Size& image_size)
			{
				int max_basis_char_count = 0;

				auto basis_str = to_wstring(basis);

				if (max_basis_char_count < basis_str.size()) {
					max_basis_char_count = basis_str.size();
				}

				auto expanded_ratio = max_basis_char_count < 10 ? 1.0 : 0.2;
				auto down_lines = find_down_lines(std::make_tuple(std::get<0>(basis), std::get<1>(basis), cv::Range()), blocks, 0.15, expanded_ratio, 5);

				if (!down_lines.empty()) {
					down_lines.emplace(std::begin(down_lines), basis);

					for (auto i = 1; i < down_lines.size(); i++) {
						auto rect1 = to_rect(down_lines[i - 1]);
						auto rect2 = to_rect(down_lines[i]);

						if (rect1.br().y + rect1.height * 1.5 < rect2.y) {
							down_lines.resize(i - 1);
							break;
						}
					}

					if (!down_lines.empty()) {
						for (auto it = std::rbegin(down_lines); it != std::rend(down_lines); it++) {
							const auto country = preprocess_country(*it, configuration, L"countries");

							if (!std::get<1>(country).empty()) {
								const auto country_code = create_postprocess_function(configuration, L"country-countrycode.csv")(std::get<1>(country));
								if (!country_code.empty()) {
									return country_code;
								}
							}
						}
					}
				}

				return L"";
			}

			static int get_distance_threshold(const std::wstring a)
			{
				return a.size() > 5 ? 2 : a.size() > 3 ? 1 : 0;
			}

			static std::wstring
				preprocess_remove_phone_number(const std::wstring& a)
			{
				return boost::regex_replace(a, boost::wregex(L"(?:\\d{2,3})[-) ]?(?:\\d{3,4})[- ]?(?:\\d{4})"), L"");
			}

			static std::pair<cv::Rect, std::wstring>
				preprocess_port_name(const std::pair<cv::Rect, std::wstring>& a, const configuration& configuration, const std::wstring& category)
			{
				auto text = std::get<1>(a);
				text = boost::regex_replace(text, boost::wregex(L"[^a-zA-Z0-9\\., ]"), L"");
				text = boost::regex_replace(text, boost::wregex(L"14"), L"N");
				text = boost::regex_replace(text, boost::wregex(L"1"), L"I");
				text = boost::regex_replace(text, boost::wregex(L"8"), L"B");
				text = boost::regex_replace(text, boost::wregex(L"0"), L"O");
				text = boost::regex_replace(text, boost::wregex(L"EUROPEAN", boost::regex::icase), L"");
				text = boost::regex_replace(text, boost::wregex(L"SOUTH (KOREA)N?", boost::regex::icase), L"$1");
				text = boost::regex_replace(text, boost::wregex(L"(?:SEA|AIR) ?PORT", boost::regex::icase), L"PORT");
				text = boost::regex_replace(text, boost::wregex(L"PORT IN", boost::regex::icase), L"PORT");
				text = boost::trim_copy(text);

				std::vector<std::wstring> words;
				boost::algorithm::split(words, text, boost::is_any_of(L"., "), boost::token_compress_on);

				bool is_singapore = false;

				if (words.size() > 1) {
					const auto dictionary = build_spell_dictionary(configuration, category);
					for (auto i = 0; i < words.size(); i++) {
						const auto suggested = dictionary->Correct(words[i]);

						if (!suggested.empty() && suggested[0].distance <= 1) {
							if (suggested[0].term == L"singapore")
								is_singapore = true;
							words.resize(i);
							break;
						}
					}

					boost::remove_erase_if(words, [](const std::wstring& word) {
						return word == L"PORT" || word.empty();
					});

					text = boost::algorithm::join(words, L"");
				}

				if (is_singapore)
					return std::make_pair(std::get<0>(a), L"SINGAPORE");

				return std::make_pair(std::get<0>(a), text);
			}

			static std::pair<cv::Rect, std::wstring>
				preprocess_airport_name(const std::pair<cv::Rect, std::wstring>& a, const configuration& configuration, const std::wstring& category)
			{
				auto text = std::get<1>(a);
				text = boost::regex_replace(text, boost::wregex(L"[^a-zA-Z0-9\\., ]"), L"");
				text = boost::regex_replace(text, boost::wregex(L"14"), L"N");
				text = boost::regex_replace(text, boost::wregex(L"1"), L"I");
				text = boost::regex_replace(text, boost::wregex(L"8"), L"B");
				text = boost::regex_replace(text, boost::wregex(L"0"), L"O");
				text = boost::regex_replace(text, boost::wregex(L"(?:INTL|INTERNATIONAL)"), L"");
				text = boost::trim_copy(text);

				std::vector<std::wstring> words;
				boost::algorithm::split(words, text, boost::is_any_of(L"., "), boost::token_compress_on);

				bool is_singapore = false;

				if (words.size() > 1) {
					const auto dictionary = build_spell_dictionary(configuration, category);
					for (auto i = 0; i < words.size(); i++) {
						const auto suggested = dictionary->Correct(words[i]);

						if (!suggested.empty() && suggested[0].distance <= 1) {
							if (suggested[0].term == L"singapore")
								is_singapore = true;
							words.resize(i);
							break;
						}
					}

					boost::remove_erase_if(words, [](const std::wstring& word) {
						return word == L"AIRPORT" || word.empty();
					});

					text = boost::algorithm::join(words, L"");
				}

				if (is_singapore)
					return std::make_pair(std::get<0>(a), L"SINGAPORE");

				return std::make_pair(std::get<0>(a), text);
			}

			static std::pair<cv::Rect, std::wstring>
				preprocess_country_port_name(const std::pair<cv::Rect, std::wstring>& a, const configuration& configuration, const std::wstring& category, bool cleaned)
			{
				auto text = std::get<1>(a);
				text = boost::regex_replace(text, boost::wregex(L"[^a-zA-Z0-9\\., ]"), L"");
				text = boost::regex_replace(text, boost::wregex(L"14"), L"N");
				text = boost::regex_replace(text, boost::wregex(L"1"), L"I");
				text = boost::regex_replace(text, boost::wregex(L"8"), L"B");
				text = boost::regex_replace(text, boost::wregex(L"0"), L"O");
				text = boost::regex_replace(text, boost::wregex(L"(?:SEA|AIR) ?PORT", boost::regex::icase), L"PORT");
				if (cleaned) {
					text = boost::regex_replace(text, boost::wregex(L"PORT$", boost::regex::icase), L"");
					text = boost::regex_replace(text, boost::wregex(L"^ANY", boost::regex::icase), L"");
				}
				text = boost::trim_copy(text);

				std::vector<std::wstring> words;
				boost::algorithm::split(words, text, boost::is_any_of(L"., "), boost::token_compress_on);

				const auto countries = get_dictionary_words(configuration, dictionaries_, category);
				const auto dictionary = build_spell_dictionary(configuration, category);
				aho_corasick::wtrie trie;
				build_trie(trie, countries);

				for (auto& word : words) {
					const auto suggested = dictionary->Correct(word);
					if (!suggested.empty() && suggested[0].distance <= 1)
						word = suggested[0].term;
				}

				text = boost::algorithm::join(words, L" ");

				return std::make_pair(std::get<0>(a), text);
			}

			static std::pair<cv::Rect, std::wstring>
				preprocess_vessel_name(const std::pair<cv::Rect, std::wstring>& a)
			{
				auto text = std::get<1>(a);
				text = boost::regex_replace(text, boost::wregex(L"\\|"), L"1");
				text = boost::regex_replace(text, boost::wregex(L"L1NE"), L"LINE");
				text = boost::regex_replace(text, boost::wregex(L"/?FLIGHT to:?", boost::regex::icase), L"");

				return std::make_pair(std::get<0>(a), text);
			}

			static std::pair<cv::Rect, std::wstring>
				preprocess_company(const std::pair<cv::Rect, std::wstring>& a, const configuration& configuration, const std::wstring& category)
			{
				auto text = std::get<1>(a);
				text = boost::regex_replace(text, boost::wregex(L"[^a-zA-Z0-9&\\., ()/\\n-]"), L"");
				text = boost::regex_replace(text, boost::wregex(L"(\\s)\\s+"), L"$1");
				text = boost::regex_replace(text, boost::wregex(L"\\s*([\\.,])\\s*"), L"$1");
				text = boost::regex_replace(text, boost::wregex(L"\\s+(\\S)\\s+"), L"$1 ");
				text = boost::regex_replace(text, boost::wregex(L"(?:^.*\\s+TO|^TO)\\s+", boost::regex::icase), L"");
				text = boost::regex_replace(text, boost::wregex(L"(llc|ltd|corp|inc)$", boost::regex::icase), L"$1");
				text = boost::regex_replace(text, boost::wregex(L"AS CARRIER", boost::regex::icase), L"");
				text = boost::trim_copy(text);

				std::vector<std::wstring> words;
				boost::algorithm::split(words, text, boost::is_any_of(L" "), boost::token_compress_on);

				const auto keywords = get_dictionary_words(configuration, dictionaries_, category);
				const auto dictionary = build_spell_dictionary(configuration, category);
				aho_corasick::wtrie trie;
				build_trie(trie, keywords);

				if (words.size() > 1) {
					auto& last_word = words.back();
					if (last_word.find(L'\n') == std::wstring::npos) {
						const auto suggested = dictionary->Correct(last_word);

						if (!suggested.empty() && suggested[0].distance <= get_distance_threshold(last_word))
							last_word = suggested[0].term;
					}
					else {
						std::vector<std::wstring> cleaned_words;
						boost::algorithm::split(cleaned_words, last_word, boost::is_any_of(L"\n"));

						for (auto& word : cleaned_words) {
							const auto suggested = dictionary->Correct(word);
							if (!suggested.empty() && suggested[0].distance <= 1)
								word = suggested[0].term;
						}

						last_word = boost::algorithm::join(cleaned_words, L"\n");
					}

					text = boost::algorithm::join(words, L" ");
				}

				const auto matches = trie.parse_text(text);

				if (matches.empty())
					return std::make_pair(std::get<0>(a), L"");

				const auto match = matches.back();

				if (boost::regex_search(text, boost::wregex(L"^pt\\.", boost::regex_constants::icase))) {
					return std::make_pair(std::get<0>(a),
						boost::replace_all_copy(std::wstring(text.begin() + match.get_start(), text.end()), L"\n",
						L""));
				}

				if (match.get_end() + 1 == text.size() ||
					text[match.get_end() + 1] == L'.' ||
					text[match.get_end() + 1] == L',' ||
					text[match.get_end() + 1] == L'\n' ||
					text[match.get_end() + 1] == L' ')
					return std::make_pair(std::get<0>(a),
					boost::replace_all_copy(std::wstring(text.begin(), text.begin() + match.get_end() + 1), L"\n",
					L" "));

				return std::make_pair(std::get<0>(a), L"");
			}

			static std::pair<cv::Rect, std::wstring>
				preprocess_insurance_company(const std::pair<cv::Rect, std::wstring>& a, const configuration& configuration, const std::wstring& category)
			{
				auto text = std::get<1>(a);
				text = boost::regex_replace(text, boost::wregex(L"[^\\(\\)a-zA-Z0-9&\\., \\n-]"), L"");
				text = boost::regex_replace(text, boost::wregex(L"(\\s)\\s+"), L"$1");
				text = boost::regex_replace(text, boost::wregex(L"\\s*([\\.,])\\s*"), L"$1");
				text = boost::regex_replace(text, boost::wregex(L"\\s+(\\S)\\s+"), L"$1");
				text = boost::regex_replace(text, boost::wregex(L"1"), L"I");
				text = boost::regex_replace(text, boost::wregex(L"\\s?\\&\\s?|\\sS\\s"), L" \\& ");
				text = boost::regex_replace(text, boost::wregex(L"(COMPANT|COMPEM)[\\.,]?$", boost::regex::icase), L"company");
				text = boost::regex_replace(text, boost::wregex(L"hanuuha", boost::regex::icase), L"hanwha");
				text = boost::regex_replace(text, boost::wregex(L"A\\.N\\.$", boost::regex::icase), L"A.S.");

				if (text.size() < 9 || std::count(std::begin(text), std::end(text), L' ') == 0 || boost::erase_all_copy(text, L" ").find(L"INSURANCEPLICY") != std::wstring::npos)
					return std::make_pair(std::get<0>(a), L"");

				std::vector<std::wstring> words;
				boost::algorithm::split(words, text, boost::is_any_of(L" "), boost::token_compress_on);

				const auto keywords = get_dictionary_words(configuration, dictionaries_, category);
				const auto dictionary = build_spell_dictionary(configuration, category, 1);
				aho_corasick::wtrie trie;
				build_trie(trie, keywords);

				const auto suggested = dictionary->Correct(text);
				if (!suggested.empty() && suggested[0].distance <= 1)
					text = suggested[0].term;

				const auto matches = trie.parse_text(text);

				if (matches.empty())
					return std::make_pair(std::get<0>(a), L"");

				aho_corasick::wtrie::emit_type last_matched;
				for (const auto& match : matches) {
					if (match.get_keyword().size() <= 4 || match.get_keyword().size() > 8 || match.get_keyword().find(L"marsh") != std::wstring::npos)
						last_matched = match;
				}

				if (last_matched.is_empty())
					last_matched = matches.back();

				if (last_matched.get_end() + 1 == text.size() ||
					text[last_matched.get_end() + 1] == L'.' ||
					text[last_matched.get_end() + 1] == L',' ||
					text[last_matched.get_end() + 1] == L' ' ||
					text[last_matched.get_end() + 1] == L' ') {

					if (last_matched.get_keyword().size() < 9)
						return a;

					const auto company = std::wstring(text.begin() + last_matched.get_start(), text.begin() + last_matched.get_end() + 1);
					const auto processed_company = boost::to_upper_copy(boost::trim_copy(company));

					if (processed_company == L"INSURANCE")
						return std::make_pair(std::get<0>(a), L"");
					else if (processed_company == L"INSURANCE COMPANY" || processed_company == L"SHIPPING COMPANY" || processed_company == L"SHIPPING CORPORATION") {
						auto postfix = std::wstring(std::begin(text) + last_matched.get_end() + 1, std::end(text));
						if (!postfix.empty() && postfix.find(L'.') != std::wstring::npos)
							return std::make_pair(std::get<0>(a), std::wstring(std::begin(text), std::begin(text) + last_matched.get_end() + 1 + postfix.find(L'.')));
						return std::make_pair(std::get<0>(a), text);
					}

					return std::make_pair(std::get<0>(a), company);
				}

				return std::make_pair(std::get<0>(a), L"");
			}

			//preprocess_lc_number
			static std::pair<cv::Rect, std::wstring>
				preprocess_lc_number(const std::pair<cv::Rect, std::wstring>& a)
			{
				auto text = std::get<1>(a);
				//text = boost::regex_replace(text, boost::wregex(L"\\|"), L"1");

				return std::make_pair(std::get<0>(a), text);
			}
			static std::pair<cv::Rect, std::wstring>
				preprocess_amount(const std::pair<cv::Rect, std::wstring>& a)
			{
				auto text = std::get<1>(a);
				//text = boost::regex_replace(text, boost::wregex(L"\\|"), L"1");

				return std::make_pair(std::get<0>(a), text);
			}

			static std::pair<cv::Rect, std::wstring>
				preprocess_currency_sign(const std::pair<cv::Rect, std::wstring>& a)
			{
				auto text = std::get<1>(a);
				//text = boost::regex_replace(text, boost::wregex(L"\\|"), L"1");

				return std::make_pair(std::get<0>(a), text);
			}


			static std::pair<cv::Rect, std::wstring>
				preprocess_country(const std::pair<cv::Rect, std::wstring>& a, const configuration& configuration, const std::wstring& category)
			{
				auto text = std::get<1>(a);
				text = boost::regex_replace(text, boost::wregex(L"_"), L" ");
				text = boost::regex_replace(text, boost::wregex(L"[^a-zA-Z\\., ]"), L"");
				text = boost::regex_replace(text, boost::wregex(L"(\\s)\\s+"), L"$1");
				text = boost::regex_replace(text, boost::wregex(L"\\s*([\\.,])\\s*"), L"$1");
				text = boost::regex_replace(text, boost::wregex(L"\\s+(\\S)\\s+"), L"$1");
				text = boost::regex_replace(text, boost::wregex(L"(?:^.*\\s+TO|^TO)\\s+", boost::regex::icase), L"");

				std::vector<std::wstring> words;
				boost::algorithm::split(words, text, boost::is_any_of(L",. "), boost::token_compress_on);

				const auto keywords = get_dictionary_words(configuration, dictionaries_, category);
				const auto dictionary = build_spell_dictionary(configuration, category);
				aho_corasick::wtrie trie;
				build_trie(trie, keywords);

				if (words.size() > 1) {
					for (auto& word : words) {
						if (word.find(L'\n') == std::wstring::npos) {
							const auto suggested = dictionary->Correct(word);
							if (!suggested.empty() && suggested[0].distance <= get_distance_threshold(word))
								word = suggested[0].term;
						}
						else {
							std::vector<std::wstring> cleaned_words;
							boost::algorithm::split(cleaned_words, word, boost::is_any_of(L"\n"));

							for (auto& word : cleaned_words) {
								const auto suggested = dictionary->Correct(word);
								if (!suggested.empty() && suggested[0].distance <= 1)
									word = suggested[0].term;
							}

							word = boost::algorithm::join(cleaned_words, L"\n");
						}
					};

					text = boost::algorithm::join(words, L" ");
				}

				const auto matches = trie.parse_text(text);

				if (!matches.empty())
					return std::make_pair(to_rect(a), std::wstring(text.begin() + matches[0].get_start(),
					text.begin() + matches[0].get_end() + 1));

				return std::make_pair(std::get<0>(a), L"");
			}

			static std::pair<cv::Rect, std::wstring>
				preprocess_good_description(const std::pair<cv::Rect, std::wstring>& a)
			{
				auto good_description = to_wstring(a);

				good_description = boost::regex_replace(good_description, boost::wregex(L"[^ a-zA-Z0-9&-]"), L"");
				boost::trim(good_description);

				return std::make_pair(to_rect(a), good_description);
			}

			static std::wstring
				postprocess_vessel_name(const std::wstring& a, const configuration& configuration)
			{
				auto text = boost::regex_replace(a, boost::wregex(L"^M\\s*\\.\\s*V\\s*.\\s", boost::regex_constants::icase), L"");
				text = boost::regex_replace(text, boost::wregex(L"[^/# 0-9A-Z\\.-]", boost::regex_constants::icase), L"");

				if (text.size() > 60)
					return L"";

				text = boost::regex_replace(text, boost::wregex(L"(.*) SAILING ON.*", boost::regex_constants::icase), L"$1");
				text = boost::regex_replace(text, boost::wregex(L"V\\.s+", boost::regex_constants::icase), L"V.");
				text = boost::regex_replace(text, boost::wregex(L"\\s+"), L" ");
				text = boost::trim_copy(text);

				if (std::count_if(std::begin(text), std::end(text), std::isalpha) < 3)
					return L"";

				if (text.size() > 45 || text.size() < 4)
					return L"";

				if (!classify_text(text, get_weights(configuration, weights_, L"vessel"), 0.5)) {
					const auto dictionary = get_dictionary_words(configuration, dictionaries_, L"vessels");

					aho_corasick::wtrie trie;
					trie.case_insensitive().remove_overlaps().allow_space();
					build_trie(trie, dictionary);

					const auto matches = trie.parse_text(text);

					if (matches.empty())
						return L"";

					if (!matches.empty()) {
						const auto match = matches[0];
						if (match.get_start() != 0 ||
							(match.get_end() + 1 != text.size() && text[match.get_end() + 1] != L' ' && text[match.get_end() + 1] != L'.' && text[match.get_end() + 1] != L'V'))
							return L"";
					}
				}

				return boost::algorithm::trim_copy(boost::to_upper_copy(text));
			}

			static std::wstring
				postprocess_lc_number(const std::wstring& a, const configuration& configuration)
			{
				auto text = boost::regex_replace(a, boost::wregex(L"[^ /a-z0-9A-Z&]", boost::regex_constants::icase), L"");
				text = boost::regex_replace(text, boost::wregex(L"date", boost::regex_constants::icase), L"");
				text = boost::regex_replace(text, boost::wregex(L"of", boost::regex_constants::icase), L"");
				text = boost::regex_replace(text, boost::wregex(L"issue", boost::regex_constants::icase), L"");

				if (text.find(L"L/C") != -1)
				{
					return L"";
				}

				auto split_idx = text.find(L"&");
				if (split_idx > 10) {
					text = text.substr(0, split_idx);
				}
				else if (split_idx > 0 && split_idx < 10 && text.size() > split_idx + 11)
				{
					text = text.substr(split_idx);
				}
				text = boost::trim_copy(text);

				if (text.size() > 45 || text.size() < 6)
					return L"";


				return boost::algorithm::trim_copy(boost::to_upper_copy(text));
			}

			static std::wstring
				postprocess_license_number(const std::wstring& a, const configuration& configuration)
			{
				auto text = boost::regex_replace(a, boost::wregex(L"[^ /a-z0-9A-Z&]", boost::regex_constants::icase), L"");
				text = boost::regex_replace(text, boost::wregex(L"date", boost::regex_constants::icase), L"");
				text = boost::regex_replace(text, boost::wregex(L"of", boost::regex_constants::icase), L"");
				text = boost::regex_replace(text, boost::wregex(L"issue", boost::regex_constants::icase), L"");

				if (text.find(L"L/C") != -1)
				{
					return L"";
				}

				auto split_idx = text.find(L"&");
				if (split_idx > 10) {
					text = text.substr(0, split_idx);
				}
				else if (split_idx > 0 && split_idx < 10 && text.size() > split_idx + 11)
				{
					text = text.substr(split_idx);
				}
				text = boost::trim_copy(text);

				if (text.size() > 45 || text.size() < 6)
					return L"";


				return boost::algorithm::trim_copy(boost::to_upper_copy(text));
			}

			static std::wstring
				postprocess_currency_sign(const std::wstring& a, const configuration& configuration)
			{
				auto text = boost::regex_replace(a, boost::wregex(L"[^a-zA-Z\\$￥]", boost::regex_constants::icase), L"");
				auto digit_text = boost::regex_replace(a, boost::wregex(L"[^0-9.,]", boost::regex_constants::icase), L"");
				auto chk = text.find(L"￥");
				if (chk >= 0 && chk < text.size())
				{
					return L"￥";
				}

				text = boost::trim_copy(text);

				const auto dictionary = get_dictionary_words(configuration, dictionaries_, L"currency");

				aho_corasick::wtrie trie;
				trie.case_insensitive().remove_overlaps().allow_space();
				build_trie(trie, dictionary);

				const auto matches = trie.parse_text(text);

				if (matches.empty())
					return L"";


				if (text.size() == 2 && text.compare(L"US") == 0)
				{
					text = boost::regex_replace(text, boost::wregex(L"S", boost::regex_constants::icase), L"$");
				}

				if (!matches.empty()) {
					const auto match = matches[0];
					if (match.get_start() != 0 ||
						(match.get_end() + 1 != text.size() && text[match.get_end() + 1] != L' ' && text[match.get_end() + 1] != L'.' && text[match.get_end() + 1] != L'V'))
						if (digit_text.size() > a.size()*0.5) {
							return text.substr(match.get_start(), match.size());
						}
						else {
							return L"";
						}
				}

				return boost::algorithm::trim_copy(boost::to_upper_copy(text));
			}

			static std::wstring
				postprocess_amount(const std::wstring& a, const configuration& configuration)
			{
				auto text = boost::regex_replace(a, boost::wregex(L"[^ a-zA-Z\\$￥0-9.,]", boost::regex_constants::icase), L"");

				auto chk = text.find(L"￥");

				//text = boost::trim_copy(text);

				const auto dictionary = get_dictionary_words(configuration, dictionaries_, L"currency");

				aho_corasick::wtrie trie;
				trie.case_insensitive().remove_overlaps().allow_space();
				build_trie(trie, dictionary);

				const auto matches = trie.parse_text(text);

				if (matches.empty() && chk == -1)
					return L"";


				if (!matches.empty() || chk != -1) {
					const auto match = matches[matches.size() - 1];
					auto substr_start_idx = match.get_start() + match.size();
					text = text.substr(substr_start_idx);
					text = boost::regex_replace(text, boost::wregex(L"[^0-9.,]", boost::regex_constants::icase), L"");
					text = boost::trim_copy(text);

					text = boost::regex_replace(text, boost::wregex(L"[.]", boost::regex_constants::icase), L",");
					auto comma_idx = text.rfind(L",");
					if (text.size() - 1 - comma_idx == 2)
					{
						text[comma_idx] = L'.';
						//text[comma_idx] 을 L"." 으로 대체
					}
					else if (text.size() - 1 - comma_idx == 5)
					{
						text.insert(comma_idx + 4, L".");
						//text[comma_idx + 3] 에 L"." 추가
					}


					return text;
				}

				return boost::algorithm::trim_copy(boost::to_upper_copy(text));
			}

			static std::wstring
				postprocess_goods_description(const std::wstring& a, const configuration& configuration)
			{
				auto text = boost::regex_replace(a, boost::wregex(L"(?:NAME|OF GOODS)\\s*(?:-|:)?\\s*", boost::regex_constants::icase), L"");
				text = boost::regex_replace(text, boost::wregex(L"(?:\\s*-?\\s*\\d+\\.?\\d+\\s*(?:MTS?|QTY|KGS|KGM)\\s?(?:OF)?|\\s*-?\\s*(?:MTS?|QTY|QUANTITY|KGS|KGM)\\s*\\d+\\.?\\d+/\\d+\\s?UNITS? OF\\s?)", boost::regex_constants::icase), L"");

				if (text.size() > 60 || text.size() < 3)
					return L"";

				if (!classify_text(text, get_weights(configuration, weights_, L"goods"), 0.4)) {
					const auto dictionary = get_dictionary_words(configuration, dictionaries_, L"goods");

					aho_corasick::wtrie trie;
					trie.case_insensitive().remove_overlaps().allow_space();
					build_trie(trie, dictionary);

					const auto matches = trie.parse_text(text);

					if (matches.empty())
						return L"";

					return boost::algorithm::trim_copy(boost::to_upper_copy(text));
				}

				if (boost::regex_search(text, boost::wregex(L"DATE|INVOICE|AMOUNT|INSURE|AS ATTACHED\\s|(?:TERMS|PLACE) OF", boost::regex_constants::icase)))
					return L"";

				if (boost::regex_search(text, boost::wregex(L"^ITEM$", boost::regex_constants::icase)))
					return L"";

				return boost::algorithm::trim_copy(boost::to_upper_copy(text));
			}
		};

		class document_recognizer : public recognizer
		{
		public:
			std::pair<std::wstring, int>
				recognize(const std::string& buffer, const std::wstring& type, const std::string& secret) override
			{
				throw std::logic_error("not implmented");
			}
			std::unordered_map<std::wstring, std::vector<std::wstring>>
				recognize(const boost::filesystem::path& path,
				const std::wstring& class_name) override
			{
				throw std::logic_error("not implmented");
			}
			std::pair<std::wstring, int>
				recognize(const std::string& buffer, int languages, const std::string& secret) override
			{
				auto configuration = load_configuration(L"document");

				CComPtr<FREngine::IEngineLoader> loader;
				FREngine::IEnginePtr engine;
				std::tie(loader, engine) = get_engine_object(configuration);

				memory_reader memory_reader(buffer, secret);
				std::vector<block> blocks;
				std::vector<std::vector<int>> confidences;
				try {
					auto document = engine->CreateFRDocument();
					document->AddImageFileFromStream(&memory_reader, nullptr, nullptr, nullptr, L"");

					if (document->Pages->Count < 1) {
						document->Close();
						return std::make_pair(L"", 0);
					}

					switch (languages) {
					case 0:
						configuration.at(L"engine").emplace(std::make_pair(L"languages", L"English,KoreanHangul"));
						break;
					case 1:
						configuration.at(L"engine").emplace(std::make_pair(L"languages", L"English"));
						break;
					case 2:
						configuration.at(L"engine").emplace(std::make_pair(L"languages", L"KoreanHangul"));
						break;
					case 3:
						configuration.at(L"engine").emplace(std::make_pair(L"languages", L"Digits"));
						break;
					}

					auto page_preprocessing_params = engine->CreatePagePreprocessingParams();
					page_preprocessing_params->CorrectOrientation = VARIANT_TRUE;
					auto recognizer_params = engine->CreateRecognizerParams();
					if (configuration.at(L"engine").find(L"languages") != configuration.at(L"engine").end())
						recognizer_params->SetPredefinedTextLanguage(configuration.at(L"engine").at(L"languages").c_str());
					document->Preprocess(page_preprocessing_params, nullptr, recognizer_params, nullptr);


					cv::Size image_size;
					std::tie(blocks, confidences, image_size) = recognize_document(engine, configuration, document, true, true, false);
					document->Close();
				}
				catch (_com_error& e) {
				}

				release_engine_object(std::make_pair(loader, engine));

				return std::make_pair(to_wstring(blocks), to_confidence(confidences));
			}

			std::unordered_map<std::wstring, std::vector<std::wstring>>
				recognize(const boost::filesystem::path& path,
				const std::string& secret) override
			{
				throw std::logic_error("not implmented");
			}
		};


		class idcard_document_recognizer : public recognizer
		{
		public:
			typedef struct tagCFineTextCharacter {
				WCHAR Unicode;          /* Character code as defined by Unicode standard */
				WORD SmallLetterHeight; /* Height of small letter for detected font */
				RECT Rect;              /* Bounding rectangle of character/ligature */
				unsigned int Attributes;       /* Combination of FCA_* flags */
				BYTE Quality;           /* Character recognition quality (from FTCQ_Min to FTCQ_Max) */
				BYTE Reserved1;         /* Reserved */
				BYTE Reserved2;         /* Reserved */
				BYTE Reserved3;         /* Reserved */
				tagCFineTextCharacter() {
					Unicode = L' ';
					SmallLetterHeight = 0;
					Rect.left = Rect.top = Rect.right = Rect.bottom = 0;
					Attributes = 0;
					Quality = Reserved1 = Reserved2 = Reserved3 = 0;
				}
			} CFineTextCharacter;

			using Line = std::vector<CFineTextCharacter>;
			using Block = std::vector<Line>;
			using Layout = std::vector<Block>;

			std::pair<std::wstring, int>
				recognize(const std::string& buffer, const std::wstring& type, const std::string& secret) override
			{
				auto configuration = load_configuration(L"idcard");

				try {
					cv::TickMeter recognition_ticks;
					recognition_ticks.start();

					CComPtr<FREngine::IEngineLoader> loader;
					FREngine::IEnginePtr engine;
					std::tie(loader, engine) = get_engine_object(configuration);
					recognition_ticks.stop();
					SV_LOG("service", spdlog::level::info, "get_engine_object time : {:.2f}Sec", recognition_ticks.getTimeSec());

					memory_reader memory_reader(buffer, secret);

					auto document = engine->CreateFRDocument();
					document->AddImageFileFromStream(&memory_reader, nullptr, nullptr, nullptr, L"");

					if (document->GetPages()->GetCount() < 1) {
						document->Close();
						return std::make_pair(L"", 0);
					}

					configuration.at(L"engine").emplace(std::make_pair(L"languages", L"Korean,Digits"));

					engine->CleanRecognizerSession();
					engine->LoadPredefinedProfile(configuration.at(L"engine").at(L"profile").c_str());

					auto profile_path = boost::filesystem::path("preset.ini");
					if (!boost::filesystem::exists(profile_path))
						profile_path = boost::filesystem::path(fmt::format(L"{}\\bin\\preset.ini", get_install_path()));
					engine->LoadProfile(boost::filesystem::absolute(profile_path).native().c_str());

					if (document->GetPages()->Item(0)->GetLayout() != nullptr)
						document->GetPages()->Item(0)->GetLayout()->Clean();

					auto recognizer_params = engine->CreateRecognizerParams();
					if (configuration.at(L"engine").find(L"languages") != configuration.at(L"engine").end())
						recognizer_params->SetPredefinedTextLanguage(configuration.at(L"engine").at(L"languages").c_str());

					recognition_ticks.start();
					document->Analyze(nullptr, nullptr, recognizer_params);
					document->Recognize(nullptr, nullptr);
					recognition_ticks.stop();
					SV_LOG("service", spdlog::level::info, "Analyze/Recognize time : {:.2f}Sec", recognition_ticks.getTimeSec());

					cv::TickMeter recognition_ticks2;
					recognition_ticks2.start();
					auto data = to_layout(document->GetPages()->Item(0)->GetPlainText());
					recognition_ticks2.stop();
					SV_LOG("service", spdlog::level::info, "extract_data_from_layout time : {:.2f}Sec", recognition_ticks2.getTimeSec());
					if (data.empty()) {
						document->Close();
						return std::make_pair(L"", 0);
					}

					merge_blocks(data);
					erase_duplicated_chars(data);

					Block all_lines;
					for (auto& block : data) {
						for (auto& line : block) {
							all_lines.emplace_back(line);
						}
					}
					sort_lines(all_lines);

					std::wstring result_path = get_result_image(document, configuration, type);
					cv::Mat image = cv::imread(to_cp949(result_path).c_str());
					if (type == L"idcard") {
						merge_lines_with_small_height(all_lines);
						auto all_words = divide_to_word_for_RRC(all_lines, 2.2);

						Block lines;
						const std::wstring reg(L"[0-9]{2}(0[1-9]|1[012])(0[1-9]|1[0-9]|2[0-9]|3[01])[\"\? ]{0,1}[-:~=]{1}[ 01234]{0,1}");
						find_text_lines_from_regex_for_RRC(all_lines, reg, document->GetPages()->Item(0)->GetImageDocument()->GetGrayImage()->GetWidth(), lines); // (아마도) 표를 구성하는 선이 1 및 - 으로 인식되어 못 찾는 경우 발생

						std::vector<Line> id_lines;
						int dist = document->GetPages()->Item(0)->GetImageDocument()->GetGrayImage()->GetHeight();
						for (auto& line : lines) {
							auto candidate_block = to_rect(line);

							if (candidate_block.height < 6 || candidate_block.y > dist) {
								continue;
							}
							id_lines.emplace_back(line);
							dist = candidate_block.y + candidate_block.height * 5;
						}

						for (auto& line : id_lines) {
							cv::rectangle(image, to_rect(line), cv::Scalar(0, 0, 0), CV_FILLED);
						}
					} else if (type == L"fr") {
						auto all_words = divide_to_word_for_family(all_lines, 1);
						sort_lines(all_words);

						auto first_line = find_keywords(all_words, L"주민등록번호", 0.2);

						const std::wstring reg(L"[0-9]{2}(0[1-9]|1[012])(0[1-9]|1[0-9]|2[0-9]|3[01])[\"\? ]{0,1}[-:~]{0,1}[ 01234]{0,1}");
						auto num_lines = find_text_lines_from_regex(all_words, reg);
						auto key_rect = to_rect(first_line);

						auto image_height = document->GetPages()->Item(0)->GetImageDocument()->GetGrayImage()->GetHeight();
						auto image_width = document->GetPages()->Item(0)->GetImageDocument()->GetGrayImage()->GetWidth();

						Block num_block;
						for (auto& line : num_lines) {
							if (to_rect(line).y > image_height * 0.75 || to_rect(line).x < image_width / 3) {
								continue;
							}

							auto r = to_rect(line);
							if (key_rect.area() > 0) {
								key_rect.y = r.y;
								if ((r & key_rect).area() > r.area() * 0.5)
									num_block.push_back(line);
							} else {
								if (r.x > image_width / 3)
									num_block.push_back(line);
							}
						}

						for (auto line : num_block) {
							/*int letter_width = line[5].Rect.right - line[0].Rect.left;
							auto id_box = to_rect(line);
							id_box.width = int(letter_width * 2.5);*/
							cv::rectangle(image, to_rect(line), cv::Scalar(0, 0, 0), CV_FILLED);
						}
					}
					cv::imwrite(to_cp949(result_path).c_str(), image);

					document->Close();

					release_engine_object(std::make_pair(loader, engine));
				} catch (_com_error& e) {
					spdlog::get("recognizer")->error("exception : {} : ({} : {})", to_cp949(e.Description().GetBSTR()), __FILE__, __LINE__);
				}


				return std::make_pair(L"", 0);
			}

			std::pair<std::wstring, int>
				recognize(const std::string& buffer, int languages, const std::string& secret) override
			{
				throw std::logic_error("not implmented");
			}
			std::unordered_map<std::wstring, std::vector<std::wstring>>
				recognize(const boost::filesystem::path& path, const std::string& secret) override
			{
				throw std::logic_error("not implmented");
			}

			std::unordered_map<std::wstring, std::vector<std::wstring>>
				recognize(const boost::filesystem::path& path,
					const std::wstring& class_name) override
			{
				throw std::logic_error("not implmented");
			}
		private:
			static std::wstring get_result_image(const FREngine::IFRDocumentPtr document, const configuration& configuration, const std::wstring& type) {
				std::wstring storage_path = fmt::format(L"{}\\{}", get_storage_path(configuration), type);
				if (!boost::filesystem::exists(storage_path))
					boost::filesystem::create_directories(storage_path);

				std::time_t time = std::time(nullptr);
				std::tm *tm = std::localtime(&time);
				std::wostringstream filename;
				filename << std::put_time(tm, L"%Y%m%d%H%M%S");
				std::wstring path = fmt::format(L"{}\\{}.jpeg", storage_path, filename.str());

				auto ori_image = document->GetPages()->Item(0)->GetImageDocument()->GetColorImage();
				ori_image->WriteToFile(BSTR(path.c_str()), FREngine::IFF_JpegColorJfif, nullptr, nullptr);

				return path;
			}

			static CFineTextCharacter
				to_CFine_character(const wchar_t ch, int left, int top, int right, int bottom)
			{
				static const std::unordered_map<wchar_t, wchar_t> CONVERSIONS{
					//특수문자
					std::make_pair(L'◆', L'-'),
					std::make_pair(L'：', L':'),
					std::make_pair(L'、', L','),
					std::make_pair(L'■', L' '),
					std::make_pair(L'­­-', L'-'),
					std::make_pair(L'―', L'-'),
					std::make_pair(0xff3b, L'['),
					std::make_pair(0xff3d, L']'),
					std::make_pair(0xff08, L'('),
					std::make_pair(0xff09, L')'),
					std::make_pair(0xfffc, L' '),
					std::make_pair(0x005e, L' '),
				};
				auto character = ch;
				if (CONVERSIONS.find(character) != CONVERSIONS.end())
					character = CONVERSIONS.at(character);
				CFineTextCharacter c;
				c.Unicode = character;
				c.Rect.left = left;
				c.Rect.top = top;
				c.Rect.right = right;
				c.Rect.bottom = bottom;

				return c;// std::make_pair(cv::Rect(cv::Point(left, top), cv::Point(right, bottom)), character);
			}

			static Layout
				to_layout(const FREngine::IPlainTextPtr& plain_text)
			{
				std::vector<Block> blocks;
				std::vector<std::vector<int>> confidences;
				std::wstring text = plain_text->Text.GetBSTR();

				SAFEARRAY* page_numbers_p;
				SAFEARRAY* left_borders_p;
				SAFEARRAY* top_borders_p;
				SAFEARRAY* right_borders_p;
				SAFEARRAY* bottom_borders_p;
				SAFEARRAY* confidences_p;
				SAFEARRAY* is_suspicious_p;
				plain_text->GetCharacterData(&page_numbers_p, &left_borders_p, &top_borders_p, &right_borders_p, &bottom_borders_p, &confidences_p, &is_suspicious_p);

				SafeArrayDestroy(page_numbers_p);
				SafeArrayDestroy(is_suspicious_p);

				CComSafeArray<int> left_borders(left_borders_p);
				CComSafeArray<int> top_borders(top_borders_p);
				CComSafeArray<int> right_borders(right_borders_p);
				CComSafeArray<int> bottom_borders(bottom_borders_p);
				CComSafeArray<int> c_confidences(confidences_p);

				wchar_t word_break = L' ';
				wchar_t word_break2 = L'\t';
				wchar_t line_break = 0x2028;
				wchar_t line_break2 = 0x00AC;
				wchar_t paragraph_break = 0x2029;
				Block block;
				Line line;
				//Word word;
				std::vector<int> confidence;
				for (auto i = 0; i < text.size(); i++) {
					auto& c = text[i];
					if (c == paragraph_break) {
						//if (!word.empty())
							//line.emplace_back(word);
						if (!line.empty())
							block.emplace_back(line);
						if (!block.empty()) {
							blocks.emplace_back(block);
							confidences.emplace_back(confidence);
						}
						//word.clear();
						line.clear();
						block.clear();
					} else if (c == line_break || c == line_break2) {
						//if (!word.empty())
							//line.emplace_back(word);
						if (!line.empty())
							block.emplace_back(line);
						//word.clear();
						line.clear();
					} else {
						line.emplace_back(to_CFine_character(c, left_borders[i], top_borders[i], right_borders[i], bottom_borders[i]));
					}
				}
				return blocks;
			}

			static Layout extract_data_from_layout(const FREngine::IEnginePtr& engine, const FREngine::ILayoutPtr layout) {
				Layout extracted_data;

				FREngine::ILayoutBlocksPtr blocks = layout->GetBlocks();
				for (int block_idx = 0; block_idx < blocks->GetCount(); block_idx++) {
					FREngine::IBlockPtr block = blocks->Item(block_idx);

					if (block->GetType() == FREngine::BT_Text) {
						extract_data_from_block(engine, block, extracted_data);
					} else if (block->GetType() == FREngine::BT_Table) {
						extract_data_from_cell(engine, block, extracted_data);
					}
				}
				return extracted_data;
			}

			static void extract_data_from_block(const FREngine::IEnginePtr& engine, const FREngine::IBlockPtr block, Layout& layout) {
				Block new_block;

				auto paragraphs = block->GetAsTextBlock()->GetText()->GetParagraphs();
				for (int pidx = 0; pidx < paragraphs->GetCount(); pidx++) {
					auto paragraph = paragraphs->Item(pidx);

					FREngine::IParagraphLinesPtr lines = paragraph->GetLines();
					std::vector<int> line_chars;
					for (int lidx = 0; lidx < lines->GetCount(); lidx++) {
						auto line = lines->Item(lidx);
						line_chars.push_back(line->GetCharactersCount());
					}

					Line new_line;
					int line_index = 0;
					int char_count = 0;
					for (int i = 0; i < paragraph->GetLength(); i++) {
						auto char_params = engine->CreateCharParams();
						paragraph->GetCharParams(i, char_params);
						auto char_varis = char_params->GetCharacterRecognitionVariants();
						if (char_varis == nullptr) {
							if (char_count + 1 == line_chars[line_index]) {
								new_block.emplace_back(new_line);
								new_line.clear();
								line_index++;
								char_count = 0;
							} else {
								CFineTextCharacter character;
								character.Unicode = L' ';
								character.Quality = 100;
								if (new_line.size() > 0)
									character.Rect = new_line.back().Rect;
								int w = character.Rect.right - character.Rect.left;
								character.Rect.left += w;
								character.Rect.right += w;
								char_count++;
								new_line.emplace_back(character);
							}
							continue;
						}

						auto char_vari = char_varis->Item(char_params->GetCharacterRecognitionVariantIndex());

						CFineTextCharacter character;
						wcscpy_s(&character.Unicode, sizeof(WCHAR), (WCHAR*)char_vari->GetCharacter());
						character.Quality = char_vari->GetCharConfidence();
						character.Rect.left = char_params->GetLeft();
						character.Rect.top = char_params->GetTop();
						character.Rect.right = char_params->GetRight();
						character.Rect.bottom = char_params->GetBottom();
						new_line.emplace_back(character);
						char_count++;
					}
					new_block.emplace_back(new_line);
				}
				layout.emplace_back(new_block);
			}

			static void extract_data_from_cell(const FREngine::IEnginePtr& engine, const FREngine::IBlockPtr block, Layout& layout) {
				auto cells = block->GetAsTableBlock()->GetCells();
				for (int index = 0; index < cells->GetCount(); index++) {
					const auto block_type = cells->Item(index)->GetBlock()->GetType();
					if (block_type == FREngine::BT_Text) {
						extract_data_from_block(engine, block, layout);
					} else if (block_type == FREngine::BT_Table) {
						extract_data_from_cell(engine, block, layout);
					}
				}
			}
			static cv::Rect from(const RECT& rect)
			{
				return cv::Rect{ rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top };
			}
			static cv::Rect to_rect(const Line& line)
			{
				if (line.empty())
					return cv::Rect();

				cv::Rect rect = std::accumulate(line.begin() + 1, line.end(), from(line.front().Rect), [](const cv::Rect &r, const CFineTextCharacter &ch) {
					cv::Rect ch_rect = from(ch.Rect);
					if (ch_rect.area() < 1)
						return r;
					return r | ch_rect;
				});

				return rect;
			}
			static cv::Rect to_rect(const Block& block)
			{
				if (block.empty())
					return cv::Rect();

				cv::Rect rect = std::accumulate(block.begin() + 1, block.end(), to_rect(block.front()), [](const cv::Rect &r, const Line &line) {
					return r | to_rect(line);
				});

				return rect;
			}
			static bool is_proximity_blocks(cv::Rect& a, cv::Rect& b, const int threshold) {
				if ((abs(a.br().x - b.tl().x) < threshold || abs(a.tl().x - b.br().x) < threshold) && (abs(a.tl().y - b.tl().y) < threshold)) {
					return true;
				}
				if ((a & b).area() > 0)
					return true;
				return false;
			}
			static void sort_lines(Block& block) {
				std::sort(block.begin(), block.end(), [](const Line& line1, const Line& line2) {
					cv::Rect r1 = to_rect(line1);
					cv::Rect r2 = to_rect(line2);
					float threshold = ((r1.br().y - r1.tl().y) + (r2.br().y - r2.tl().y)) / 2 * 0.5;
					if (fabs(r1.tl().y - r2.tl().y) < threshold)
						return (r1.tl().x < r2.tl().x);
					return (r1.tl().y < r2.tl().y);
				});
			}
			static void merge_lines(Block& block) {
				for (int i = 0; i < block.size() - 1; i++) {
					auto& line1 = block[i];

					if (line1.size() < 1)
						continue;

					for (int j = i + 1; j < block.size(); ) {
						auto& line2 = block[j];
						auto rect1 = to_rect(line1);
						auto rect2 = to_rect(line2);
						auto center1 = (rect1.tl() + rect1.br()) / 2;
						auto center2 = (rect2.tl() + rect2.br()) / 2;
						if (abs(rect1.br().y - rect2.br().y) < 5) {
							CFineTextCharacter character;
							character.Unicode = L' ';
							character.Quality = 100;
							character.Rect = line1.back().Rect;
							int w = character.Rect.right - character.Rect.left;
							character.Rect.left += w;
							character.Rect.right += w;
							line1.emplace_back(character);
							line1.insert(line1.end(), line2.begin(), line2.end());
							block.erase(block.begin() + j);
						} else {
							j++;
						}
					}
				}
			}
			static void merge_blocks(Layout& layout) {
				for (int i = 0; i < layout.size() - 1; i++) {
					auto& block1 = layout[i];

					if (block1.size() < 1)
						continue;

					int threshold = to_rect(block1.front()).height;
					for (int j = i + 1; j < layout.size(); ) {
						auto& block2 = layout[j];
						if (is_proximity_blocks(to_rect(block1), to_rect(block2), threshold)) {
							block1.insert(block1.end(), block2.begin(), block2.end());
							layout.erase(layout.begin() + j);
						} else {
							j++;
						}
					}
				}
				for (auto& block : layout) {
					sort_lines(block);
					merge_lines(block);
				}
			}
			static void erase_duplicated_chars(Layout& layout) {
				for (auto& block : layout) {
					for (auto& line : block) {
						if (line.empty())
							continue;

						for (int i = 0; i < line.size() - 1; i++) {
							auto& line1 = line[i];
							auto& rect1 = from(line1.Rect);
							for (int j = i + 1; j < line.size();) {
								auto& line2 = line[j];
								auto& rect2 = from(line2.Rect);
								if ((rect1&rect2).area() > rect1.area() * 0.9 && line1.Unicode == line2.Unicode) {
									line.erase(line.begin() + j);
								} else {
									j++;
								}
							}
						}
					}
				}
			}
			static void merge_lines_with_small_height(Block& block) {
				for (int i = 0; i < block.size() - 1; i++) {
					auto& line1 = block[i];

					if (line1.size() < 1)
						continue;

					for (int j = i + 1; j < block.size(); ) {
						auto& line2 = block[j];
						auto rect1 = to_rect(line1);
						auto rect2 = to_rect(line2);
						auto center1 = (rect1.tl() + rect1.br()) / 2;
						auto center2 = (rect2.tl() + rect2.br()) / 2;
						if (abs(rect1.br().y - rect2.br().y) < 5 ||
							rect1.height > rect2.height ? (center2.y > rect1.y && center2.y < rect1.br().y) : (center1.y > rect2.y && center1.y < rect2.br().y)) {
							CFineTextCharacter character;
							character.Unicode = L' ';
							character.Quality = 100;
							character.Rect = line1.back().Rect;
							int w = character.Rect.right - character.Rect.left;
							character.Rect.left += w;
							character.Rect.right += w;
							line1.emplace_back(character);
							line1.insert(line1.end(), line2.begin(), line2.end());
							block.erase(block.begin() + j);
						} else {
							j++;
						}
					}
				}
			}
			static Block divide_to_word_for_RRC(Block& block, const int threshold) {
				Block words;

				for (auto& line : block) {
					if (line.empty())
						continue;

					Line word;
					if (line.size() > 1) {
						for (int i = 0; i < line.size() - 1; i++) {
							auto& r1 = from(line[i].Rect);
							auto& r2 = from(line[i + 1].Rect);
							auto k = to_rect(word);

							if (r1.br().x + r1.width * threshold < r2.tl().x || r1.tl().x > r2.tl().x) {
								word.push_back(line[i]);
								if (i == line.size() - 2) {
									word.push_back(line[i + 1]);
								}
								words.push_back(word);
								word.clear();
							} else {
								word.push_back(line[i]);
								if (i == line.size() - 2) {
									word.push_back(line[i + 1]);
								}
							}
						}
					} else {
						word = line;
					}
					words.push_back(word);
				}
				return words;
			}
			static Block divide_to_word_for_family(Block& block, const int threshold) {
				Block words;

				for (auto& line : block) {
					if (line.empty())
						continue;

					Line word;
					if (line.size() > 1) {
						for (int i = 0; i < line.size() - 1; i++) {
							auto& r1 = from(line[i].Rect);
							auto& r2 = from(line[i + 1].Rect);

							if (r1.br().x + (r1.width + r2.width) / 2 * threshold < r2.tl().x) {
								word.push_back(line[i]);
								if (i == line.size() - 2) {
									words.push_back(word);
									word.clear();
									word.push_back(line[i + 1]);
								}
								words.push_back(word);
								word.clear();
							} else {
								word.push_back(line[i]);
								if (i == line.size() - 2) {
									word.push_back(line[i + 1]);
								}
							}
						}
					} else {
						word = line;
					}
					words.push_back(word);
				}
				return words;
			}
			static void remove_character(std::wstring &s, wchar_t ch) {
				s.erase(std::remove_if(s.begin(), s.end(), [ch](const wchar_t& c) {
					return c == ch;
				}), s.end());
			}
			static std::wstring get_text(const Line& line) {
				std::wstringstream text;

				if (line.size() < 1)
					return text.str();

				for (const auto& character : line) {
					text << character.Unicode;
				}

				if (line.size() > 0)
					text << L"\n";

				std::wstring r = std::regex_replace(text.str(), std::wregex(L"ᅳ"), L"-");
				r = std::regex_replace(r, std::wregex(L"ㅡ"), L"-");
				r = std::regex_replace(r, std::wregex(L"一"), L"-");
				r = std::regex_replace(r, std::wregex(L"—"), L"-");
				r = std::regex_replace(r, std::wregex(L"卜"), L"1-");
				r = std::regex_replace(r, std::wregex(L"的"), L"69");
				r = std::regex_replace(r, std::wregex(L"抑"), L"80");
				remove_character(r, L'�');
				return r;
			}
			static bool is_number_line(const Line& line) {
				int count = 0;
				int all_char = line.size();
				for (auto ch : line) {
					if (ch.Unicode == L' ') {
						all_char -= 1;
						continue;
					}

					if (ch.Unicode >= L'0' && ch.Unicode <= L'9') {
						count += 1;
					}
				}
				if (all_char * 0.6 <= count) {
					return true;
				}
				return false;
			}
			static std::wstring korean_breaker(std::wstring input) {
				std::wstring result;
				for (wchar_t word : input) {
					if (word < 44032 || word>55203)
						return input;

					unsigned short c = word;
					c = c - 0xAC00;
					unsigned short head = c / (0x0015 * 0x001C);
					unsigned short mid = (c / 0x001C) % 0x0015;
					unsigned short tail = c % 0x001C;

					result.append(std::to_wstring(head + 0x1100));
					result.append(std::to_wstring(mid + 0x1161));
					if (tail != 0)
						result.append(std::to_wstring(tail + 0x11A7));
				}
				return result;
			}
			static int levenshtein_distance(const std::wstring &s1, const std::wstring &s2) {
				int s1len = s1.size();
				int s2len = s2.size();

				auto column_start = (decltype(s1len))1;

				auto column = new decltype(s1len)[s1len + 1];
				std::iota(column + column_start - 1, column + s1len + 1, column_start - 1);

				for (auto x = column_start; x <= s2len; x++) {
					column[0] = x;
					auto last_diagonal = x - column_start;
					for (auto y = column_start; y <= s1len; y++) {
						auto old_diagonal = column[y];
						auto possibilities = {
							column[y] + 1,
							column[y - 1] + 1,
							last_diagonal + (s1[y - 1] == s2[x - 1] ? 0 : 1)
						};
						column[y] = std::min(possibilities);
						last_diagonal = old_diagonal;
					}
				}
				auto result = column[s1len];
				delete[] column;
				return result;
			}
			static int string_distance(std::wstring a, std::wstring b) {
				std::wstring s1 = korean_breaker(a);
				std::wstring s2 = korean_breaker(b);
				s1 = boost::to_lower_copy(s1);
				s2 = boost::to_lower_copy(s2);
				return levenshtein_distance(s1, s2);
			}
			static bool string_contain(std::wstring a, std::wstring b, unsigned int thresh) {
				a = std::regex_replace(a, std::wregex(L"[^가-힣^ㄱ-ㅎ^0-9^A-Z^a-z]"), L"");
				b = std::regex_replace(b, std::wregex(L"[^가-힣^ㄱ-ㅎ^0-9^A-Z^a-z]"), L"");

				std::wstring qa = korean_breaker(a);
				std::wstring qb = korean_breaker(b);
				qa = boost::to_lower_copy(qa);
				qb = boost::to_lower_copy(qb);

				if (qa.find(qb) != std::string::npos) {
					return true;
				}
				if (thresh == 0)
					thresh = 1;

				if (qb.size() < 5)
					thresh = 0;
				if (a.size() >= b.size()) {
					int dif = a.size() - b.size() + 1;
					for (int i = 0; i < dif; i++) {
						std::wstring sub = a.substr(i, b.size());
						int dis = string_distance(sub, b);
						if (dis <= thresh) {
							return true;
						}
					}
				}
				else {
					int dis = string_distance(a, b);
					if (dis <= thresh) {
						return true;
					}
				}
				return false;
			}
			static Line find_keywords(Block& data, std::wstring key, const float thresh) {
				Line key_line;

				for (auto& line : data) {
					auto line_str = get_text(line);

					int threshold = key.size() * thresh;
					threshold = threshold < 1 ? 1 : threshold;
					if (string_contain(line_str, key, threshold)) {
						key_line = line;
						break;
					}
				}
				return key_line;
			}
			static Block find_text_lines_from_regex(Block& data, const std::wstring regex) {
				Block key_lines;
				for (auto& line : data) {
					auto line_str = get_text(line);
					boost::match_results<std::wstring::const_iterator> matches;
					if (boost::regex_search(line_str, matches, boost::wregex(regex))) {
						if (matches.position() >= 0) {
							key_lines.push_back(line);
						}
					}
				}
				return key_lines;
			}
			static void find_text_lines_from_regex_for_RRC(Block& data, const std::wstring regex, const int w, Block &lines) {
				lines.clear();

				Line result_line;
				for (auto& line : data) {
					result_line.clear();

					std::wstring line_str = get_text(line);
					if (is_number_line(line)) {
						line_str = std::regex_replace(line_str, std::wregex(L"아"), L"01");
					}

					boost::match_results<std::wstring::const_iterator> matches;
					if (boost::regex_search(line_str, matches, boost::wregex(regex))) {
						for (auto i = matches.position(); i < line.size(); i++) {
							if (line[i].Rect.left > w / 2 || result_line.size() > 14) {
								break;
							}

							if ((line[i].Unicode >= L'0' && line[i].Unicode <= L'9') || line[i].Unicode == L'-' ||
								line[i].Unicode >= L'卜' || line[i].Unicode >= L'的' || line[i].Unicode >= L'抑') {
								result_line.emplace_back(line[i]);
							}
						}

						if (result_line.size() < 6) {
							continue;
						}

						lines.push_back(result_line);
					}
				}
			}
		};

		void
			recognizer_factory::initialize()
		{

			const auto configuration = load_configuration();


			dictionaries_ = load_dictionaries(get_data_path(configuration));
			keywords_ = load_keywords(get_data_path(configuration));
			weights_ = load_weights(get_data_path(configuration));


			if (spdlog::get("recognizer") == nullptr) {
				spdlog::stdout_color_mt("recognizer");
			}
			else {
			}
		}

		std::unique_ptr<recognizer>
			recognizer_factory::create(const std::wstring& profile)
		{
			if (profile == L"fax")
				return std::make_unique<fax_document_recognizer>();
			else if (profile == L"trade")
				return std::make_unique<trade_document_recognizer>();
			else if (profile == L"document")
				return std::make_unique<document_recognizer>();
			else if (profile == L"coa")
				return std::make_unique<coa_document_recognizer>();
			else if (profile == L"account")
				return std::make_unique<account_recognizer>();
			else if (profile == L"court")
				return std::make_unique<court_document_recognizer>();
			else if (profile == L"idcard")
				return std::make_unique<idcard_document_recognizer>();
			return nullptr;
		}

		void
			recognizer_factory::deinitialize()
		{
			spdlog::drop("recognizer");
		}
	}
}
