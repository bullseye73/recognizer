
#include "http_server.h"
#include <Windows.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <Poco/Net/HTTPServer.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/Net/ServerSocket.h>
#include "utils.hpp"

namespace selvy
{
	namespace ocr
	{
		void SRequestHandler::handleRequest(Poco::Net::HTTPServerRequest &request, Poco::Net::HTTPServerResponse &response)
		{
			/*response.setChunkedTransferEncoding(true);
			response.setContentType("text/html");
			response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);

			std::ostream &responseStream = response.send();

			responseStream << "<html><head><head><title>HTTP Server in C++ </title></head>";
			responseStream << "<body>";
			responseStream << "<h1>Hello wordl!</h1>";
			responseStream << "<p>Count: " << ++count << "</p>";
			responseStream << "<p>Host: " << request.getHost() << "</p>";
			responseStream << "<p>Method: " << request.getMethod() << "</p>";
			responseStream << "<p>URI: " << request.getURI() << "</p>";
			responseStream << "</body></html>";
			responseStream.flush();

			std::cout << std::endl << "Response sent for count=" << count << " and URI=" << request.getURI() << std::endl;*/
		}

		void SErroRequestHandler::handleRequest(Poco::Net::HTTPServerRequest& request, Poco::Net::HTTPServerResponse& response)
		{
			//response.setChunkedTransferEncoding(true);
			//response.setContentType("text/html");  //  text/html , application/json
			//response.setStatus(Poco::Net::HTTPResponse::HTTP_NOT_FOUND);

			//std::ostream& responseStream = response.send();

			//responseStream << "<html><head><head><title>HTTP Server in C++ </title></head>";
			//responseStream << "<body><h1>PAGE NOT FOUND, SORRY!</h1><p>";
			//responseStream << "</p></body></html>";

			Poco::Util::Application& app = Poco::Util::Application::instance();
			app.logger().information("Request from " + request.clientAddress().toString());

		}

		Poco::Net::HTTPRequestHandler* SRequestHandlerFactory::createRequestHandler(const Poco::Net::HTTPServerRequest &request)
		{
			auto configuration = selvy::ocr::load_configuration();
			std::wstring path = selvy::ocr::get_root_path(configuration);

			if (request.getURI() == selvy::ocr::to_utf8(path)) {
				return new SRequestHandler();
			}

			return new SErroRequestHandler();
		}

		void SHttpServer::initialize(Poco::Util::Application &self)
		{
			Poco::Util::Application::loadConfiguration();
			Poco::Util::ServerApplication::initialize(self);
		}

		void SHttpServer::uninitialize()
		{
			Poco::Util::ServerApplication::uninitialize();
		}

		int SHttpServer::main(const std::vector<std::string> &args)
		{
			spdlog::stdout_color_mt("console");

			auto configuration = selvy::ocr::load_configuration();
			int port = selvy::ocr::get_port_number(configuration);

			Poco::Net::ServerSocket socket(port);

			Poco::Net::HTTPServerParams *pParams = new Poco::Net::HTTPServerParams();
			pParams->setMaxQueued(100);  //Sets the maximum number of queued connections.
			pParams->setMaxThreads(16);  //Sets the maximum number of simultaneous threads available for this Server

			Poco::Net::HTTPServer server(new SRequestHandlerFactory(), socket, pParams);  // Instanciate HandlerFactory

			server.start();
			std::cout << std::endl << "Server started" << std::endl;

			waitForTerminationRequest();  // wait for CTRL-C or kill

			std::cout << std::endl << "Shutting down..." << std::endl;
			server.stop();

			spdlog::drop_all();

			return EXIT_OK;
		}

	}
}
