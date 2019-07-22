#include <Windows.h>
#include <atlsafe.h>

#include <random>

#include <boost/algorithm/string.hpp>
#include <boost/range/algorithm/remove_if.hpp>
#include <boost/range/algorithm_ext/erase.hpp>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>

#include "recognizer.hpp"
#include "utils.hpp"
#include "encryption.h"

#import "FREngine.tlb"

namespace selvy
{
	namespace ocr
	{
		class memory_reader : public FREngine::IReadStream {
		public:
			ULONG ref_count_ = 0;
			std::stringstream stream_;
			std::string decrypted_;

			memory_reader(const boost::filesystem::path& file, const std::string& key)
			{
				std::ifstream ifs(file.native(), std::ios::binary | std::ios::in);

				const std::string buffer((std::istreambuf_iterator<char>(ifs)), (std::istreambuf_iterator<char>()));
				// decrypted_ = decrypt_image(buffer, key);

				if (decrypted_[0] == 'I' && decrypted_[1] == 'I') {
					/*std::istringstream istr(decrypted_);
					auto tiff_handle = TIFFStreamOpen("MemTIFF", &istr);
					istr.clear();
					if (tiff_handle) {
						tdata_t buf;
						tstrip_t strip;
						uint32* bc;
						uint32 stripsize;

						TIFFGetField(tiff_handle, TIFFTAG_STRIPBYTECOUNTS, &bc);
						stripsize = bc[0];

						buf = TIFFmalloc(stripsize);
						for (strip = 0; strip < TIFFNumberOFSrips(tiff_handle); strip++) {
							if (bc[strip] > stripsize) {
								buf = _TIFFrealloc(buf, bc[strip)]);
								stripsize = bc[strip];
							}
							TIFFReadRawStrip(tiff_handle, strip, buf, bc[strip]);
						}

						stream_.write((char *)buf, stripsize);

						decrypted_ = std::string((char*)buf, (char*)buf + stripsize);

						_TIFFfree(buf);
						TIFFClose(tiff_handle);
					}*/
				}
				else {
					// stream_ = std::stringstream(buffer);
					decrypted_ = buffer;
					stream_ = std::stringstream(buffer);
				}

#if defined(_DEBUG)
				std::vector<char> image_buffer{ std::begin(buffer), std::end(buffer) };
				auto debug_ = cv::imdecode(cv::Mat(image_buffer), cv::IMREAD_COLOR);
#endif
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

		static std::pair<CComPtr<FREngine::IEngineLoader>, FREngine::IEnginePtr>
			get_engine_object(const configuration& configuration)
		{
			CLSID cls_id;
			CLSIDFromProgID(L"FREngine.InprocLoader", &cls_id);

			CComPtr<FREngine::IEngineLoader> loader;
			FREngine::IEnginePtr engine;

			do {
				try {
					if (loader == nullptr)
						loader.CoCreateInstance(cls_id, nullptr, CLSCTX_INPROC_SERVER);

					engine = loader->InitializeEngine(configuration.at(L"engine").at(L"license").c_str(), L"", L"", L"", L"", VARIANT_FALSE);
					engine->LoadPredefinedProfile(L"TextExtraction_Speed");
				}
				catch (_com_error& e) {
					loader->ExplicitlyUnload();
					std::this_thread::sleep_for(std::chrono::seconds(1));
				}
			} while (engine == nullptr);

			return std::make_pair(loader, engine);
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

		static std::pair<std::wstring, double>
			classify_document(const FREngine::IEnginePtr& engine, const FREngine::IClassificationEnginePtr& classification_engine, const FREngine::IModelPtr& model,
			const boost::filesystem::path& file, const std::string& secret)
		{
			std::wstring category;
			double confidence;

			const auto prepare_image_mode = [&]() {
				auto pim = engine->CreatePrepareImageMode();
				return pim;
			}();

			cv::TickMeter recognizer_ticks;
			auto document = engine->CreateFRDocument();

			memory_reader memory_reader(boost::filesystem::absolute(file), secret);

			if (is_document_filtered(memory_reader.decrypted_)) {
				document->Close();
				return std::make_pair(category, confidence);
			}

			document->AddImageFileFromStream(&memory_reader, nullptr, prepare_image_mode, nullptr,
				boost::filesystem::path(file).filename().native().c_str());

			if (document->Pages->Count == 0) {
				document->Close();
				return std::make_pair(category, confidence);
			}

			const auto image_width = document->Pages->Item(0)->ImageDocument->GetColorImage()->GetWidth();
			if (image_width < 500) {
				document->Close();
				return std::make_pair(category, confidence);
			}

			const auto document_processing_params = engine->CreateDocumentProcessingParams();
			document_processing_params->PageProcessingParams->PagePreprocessingParams->CorrectOrientation = VARIANT_TRUE;
			document_processing_params->PageProcessingParams->PagePreprocessingParams->OrientationDetectionParams->put_OrientationDetectionMode
				(FREngine::OrientationDetectionModeEnum::ODM_Thorough);
			document->Process(document_processing_params);

			cv::TickMeter classifier_ticks;
			auto classification_object = classification_engine->CreateObjectFromDocument(document);

			const auto classified = model->Classify(classification_object);

			if (classified != nullptr && classified->Count != 0) {
				category = std::wstring(classified->Item(0)->CategoryLabel);
				confidence = classified->Item(0)->Probability;
			}

			document->Close();

			return std::make_pair(category, confidence);
		}

		static std::pair<int, double>
			test_classification_model(const FREngine::IEnginePtr& engine, const configuration& configuration,
			const FREngine::IClassificationEnginePtr& classification_engine,
			FREngine::IModelPtr& model, const std::vector<std::pair<std::wstring, boost::filesystem::path>>& dataset, const std::string& secret)
		{
			auto corrected_classification_count = 0;

			for (const auto& data : dataset) {
				cv::TickMeter classification_ticks;
				classification_ticks.start();
				std::wstring solution;
				boost::filesystem::path file;
				std::tie(solution, file) = data;
				const auto classification = classify_document(engine, classification_engine, model, file.native(), secret);
				classification_ticks.stop();

				auto log_level = spdlog::level::err;
				if (std::get<0>(classification) == solution) {
					++corrected_classification_count;
					log_level = spdlog::level::info;
				}

				spdlog::get("trainer")->log(log_level, "{} ({:.2f}mSec, category : {} -> {} ({:.2f}), cache : true)",
					to_cp949(file.filename().native()), classification_ticks.getTimeMilli(),
					to_cp949(solution), to_cp949(std::get<0>(classification)), std::get<1>(classification));
			}

			return std::make_pair(corrected_classification_count, static_cast<double>(corrected_classification_count) / dataset.size());
		}

		static void
			train_classification_model(const FREngine::IEnginePtr& engine, const configuration& configuration,
			const std::wstring& dataset_directory, const std::string& secret)
		{
			engine->LoadProfile(L"preset.ini");

			const auto classification_datasets = dataset_directory;
			const auto classification_directory = get_classification_directory(configuration);
			const auto classification_data = get_classification_data(configuration);
			const auto classification_model = get_classification_model(configuration);

			std::vector<std::pair<std::wstring, boost::filesystem::path>> files;
			std::vector<std::wstring> categories;

			for (auto& entry : boost::filesystem::recursive_directory_iterator(classification_datasets)) {
				const auto extension = boost::algorithm::to_lower_copy(entry.path().extension().native());

				if (extension != L".png" && extension != L".jpg" && extension != L".tif")
					continue;

				const auto class_name = entry.path().parent_path().filename().native();
				files.emplace_back(std::make_pair(class_name, boost::filesystem::absolute(entry.path())));
			}

			if (boost::filesystem::exists(classification_data)) {
				const auto classification_model_modification_date = boost::filesystem::last_write_time(classification_data);
				boost::remove_erase_if(files, [&classification_model_modification_date](
					const std::pair<std::wstring, boost::filesystem::path>& file) {
					return classification_model_modification_date > boost::filesystem::
						last_write_time(std::get<1>(file));
				});
			}

			const auto prepare_image_mode = [&]() {
				auto pim = engine->CreatePrepareImageMode();
				return pim;
			}();

#if 0
			auto classification_engine = engine->CreateClassificationEngine();
			auto trained_calssification_model = classification_engine->
				CreateModelFromFile(boost::filesystem::absolute(classification_model).native().c_str());

			const auto training_stats = test_classification_model(engine, configuration, classification_engine, trained_calssification_model, files, secret);
			spdlog::get("trainer")->info("test accuracy : {}/{} ({:.2f}%)", std::get<0>(training_stats), files.size(), std::get<1>(training_stats) * 100.);
#else
			auto classification_engine = engine->CreateClassificationEngine();
			auto classification_training_data = classification_engine->CreateTrainingData();

			if (boost::filesystem::exists(classification_data)) {
				classification_training_data->LoadFromFile(boost::filesystem::absolute(classification_data).native().c_str());
			}

			auto logger = spdlog::basic_logger_st("file", "C:\\workspace\\vs2015\\OUTPUT\\classification\\ids.txt", true);

			if (!files.empty()) {
				std::shuffle(std::begin(files), std::end(files), std::mt19937(std::random_device()()));
				const auto training_count = files.size() * 1;
				decltype (files) training_dataset(std::begin(files), std::begin(files) + training_count);
				decltype (files) testing_dataset(std::begin(files) + training_count, std::end(files));

				for (auto i = 0; i < training_dataset.size(); i++) {
					std::wcout << std::get<1>(training_dataset[i]).native() << std::endl;
					cv::TickMeter trainer_ticks;
					trainer_ticks.start();
					auto document = engine->CreateFRDocument();
					std::wstring category;
					boost::filesystem::path file;
					std::tie(category, file) = training_dataset[i];

					memory_reader memory_reader(boost::filesystem::absolute(file), secret);
					if (is_document_filtered(memory_reader.decrypted_)) {
						document->Close();
						continue;
					}

					document->AddImageFileFromStream(&memory_reader, nullptr, prepare_image_mode, nullptr,
						boost::filesystem::path(file).filename().native().c_str());

					auto page_preprocessing_params = engine->CreatePagePreprocessingParams();
					page_preprocessing_params->CorrectOrientation = VARIANT_TRUE;
					page_preprocessing_params->OrientationDetectionParams->put_OrientationDetectionMode(FREngine::OrientationDetectionModeEnum::ODM_Thorough);
					document->Preprocess(page_preprocessing_params, nullptr, nullptr, nullptr);

					if (document->Pages->Count < 1) {
						document->Close();
						continue;
					}

					document->Analyze(nullptr, nullptr, nullptr);

					if (configuration.at(L"setting").at(L"profile") == L"trade") {
						const auto height = document->Pages->Item(0)->ImageDocument->GrayImage->Height;
						const auto layout_blocks = document->Pages->Item(0)->Layout->Blocks;
						for (auto i = layout_blocks->Count - 1; i >= 0; i--) {
							const auto top = layout_blocks->Item(i)->Region->BoundingRectangle->Top;
							if (top > height / 2) {
								layout_blocks->DeleteAt(i);
							}
						}

						if (layout_blocks->Count == 0) {
							logger->info("{} : 0 blocks", to_cp949(file.native()));
							spdlog::get("trainer")->error("[{:05d}/{:05d}] {:03.2f}%({:.2f}mSec)", i + 1, files.size(), static_cast<float>(i + 1) / files.size() * 100,
								trainer_ticks.getTimeMilli());
							document->Close();
							continue;
						}
					}

					document->Recognize(nullptr, nullptr);

					auto classification_object = classification_engine->CreateObjectFromDocument(document);
					auto suitable_classifier = classification_object->SuitableClassifiers;

					if (suitable_classifier & FREngine::ClassifierTypeEnum::CT_Combined == 0) {
						logger->info("{} : {}", to_cp949(file.native()), to_cp949(classification_object->Description.GetBSTR()));
						spdlog::get("trainer")->error("[{:05d}/{:05d}] {:03.2f}%({:.2f}mSec)", i + 1, files.size(), static_cast<float>(i + 1) / files.size() * 100., trainer_ticks.getTimeMilli());
						document->Close();
						continue;
					}

					auto category_index = classification_training_data->Categories->Find(category.c_str());

					if (category_index == -1)
						classification_training_data->Categories->AddNew(category.c_str());

					category_index = classification_training_data->Categories->Find(category.c_str());

					auto category_objects = classification_training_data->Categories->Item(category_index)->Objects;
					category_objects->Add(classification_object);

					document->Close();
					trainer_ticks.stop();
					spdlog::get("trainer")->info("[{:05d}/{:05d}] {:03.2f}%({:.2f}mSec)", i + 1, files.size(), static_cast<float>(i + 1) / files.size() * 100., trainer_ticks.getTimeMilli());
				}
			}

			classification_training_data->SaveToFile(boost::filesystem::absolute(classification_data).native().c_str());

			auto classification_trainer = classification_engine->CreateTrainer();
			classification_trainer->TrainingParams->ClassifierType = FREngine::ClassifierTypeEnum::CT_Combined;
			try {
				auto training_results = classification_trainer->TrainModel(classification_training_data);
				auto training_model = training_results->Item(0)->Model;
				training_model->SaveToFile(boost::filesystem::absolute(classification_model).native().c_str());
			}
			catch (_com_error& e) {
				spdlog::get("trainer")->error("{}", to_cp949(e.Description().GetBSTR()));
			}

			auto trained_classification_model = classification_engine->
				CreateModelFromFile(boost::filesystem::absolute(classification_model).native().c_str());

			const auto training_stats = test_classification_model(engine, configuration, classification_engine, trained_classification_model, files, secret);
			spdlog::get("trainer")->info("training accuracy : {}/{} ({:.2f}%)", std::get<0>(training_stats), files.size(), std::get<1>(training_stats) * 100.);
#endif
		}

		static void
			classify_documents(const FREngine::IEnginePtr& engine, const configuration& configuration,
			const boost::filesystem::path& input_directory, const boost::filesystem::path& output_directory, const std::string& secret)
		{
			engine->LoadProfile(L"preset.ini");

			auto classification_engine = engine->CreateClassificationEngine();

			auto model = classification_engine->
				CreateModelFromFile(boost::filesystem::absolute(get_classification_model(configuration)).native().c_str());

			std::vector<boost::filesystem::path> files;
			for (const auto& file : boost::filesystem::recursive_directory_iterator(input_directory)) {
				const auto extension = boost::algorithm::to_lower_copy(file.path().extension().native());

				if (extension != L".png" && extension != L".jpg" && extension != L".tif")
					continue;

				files.emplace_back(file.path());
			}

			for (auto i = 0; i < files.size(); i++) {
				cv::TickMeter classification_ticks;
				classification_ticks.start();

				std::wstring category;
				double confidense;

				std::tie(category, confidense) = classify_document(engine, classification_engine, model, files[i].native(), secret);
				classification_ticks.stop();

				if (category.empty()) {
					spdlog::get("trainer")->info("[{:05d}/{:05d}] {} ({:.2f}mSec, category : thumbnail)", i + 1, files.size(),
						to_cp949(files[i].filename().native()), classification_ticks.getTimeMilli());
					continue;
				}

				auto level = spdlog::level::err;

				if (confidense >= 0.65) {
					level = spdlog::level::info;
				}
				else {
					category = L"ETC";
				}

				const auto category_directory = fmt::format(L"{}\\{}", output_directory.native(), category);

				if (!boost::filesystem::exists(category_directory)) {
					boost::filesystem::create_directories(category_directory);
				}

				try {
					boost::filesystem::copy(files[i], fmt::format(L"{}\\{}", category_directory, files[i].filename().native()));
				}
				catch (const boost::filesystem::filesystem_error& e) {
					spdlog::get("trainer")->error("{}", e.what());
				}

				spdlog::get("trainer")->log(level, "[{:05d}/{:05d}] {} ({:.2f}mSec, category : {} ({:.2f}))", i + 1, files.size(),
					to_cp949(files[i].filename().native()), classification_ticks.getTimeMilli(),
					to_cp949(category), confidense);
			}
		}
	}
}

int
main()
{
    spdlog::stdout_color_mt("trainer");

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    const auto configuration = selvy::ocr::load_configuration(L"trade");
    CComPtr<FREngine::IEngineLoader> loader;
    FREngine::IEnginePtr engine;
    std::tie(loader, engine) = selvy::ocr::get_engine_object(configuration);

	// const auto secret = "1selvasai@";
	const auto secret = "";
	selvy::ocr::train_classification_model(engine, configuration, L"C:\\workspace\\vs2015\\OUTPUT\\classification", secret);
	//selvy::ocr::classify_documents(engine, configuration, L"C:\\workspace\\vs2015\\images", L"C:\\workspace\\vs2015\\OUTPUT\\classification", secret);
	selvy::ocr::classify_documents(engine, configuration, L"D:\\Downloads\\IBK 관련\\images", L"C:\\workspace\\vs2015\\OUTPUT\\classification", secret);
    spdlog::drop_all();

    return 0;
}
