#pragma once
namespace duckdb {
class SparkCompatUtils {
public:
	bool static IsSparkPostfixToken(const string &str) {
		auto c = std::toupper(str[0]);
		if (str.size() == 1) {
			return (c == 'L' || c == 'S' || c == 'Y' || c == 'D' || c == 'F');
		}
		if (str.size() == 2 && c == 'B' && std::toupper(str[1]) == 'D') {
			return true;
		}
		return false;
	}
};
}