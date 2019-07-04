
#include <ctime>
#include <string>
#include <vector>

void update_request_bytes(const std::time_t timestamp, const unsigned long long &size);
void update_request_count(const std::time_t timestamp, const unsigned long long &count);
void update_recognition_type(const std::time_t timestamp, const unsigned long long &fax_count, const unsigned long long &trade_count, const unsigned long long &doc_count);

void export_graph(const std::wstring days, std::vector<std::pair<std::string, std::wstring>> &graph_list);
