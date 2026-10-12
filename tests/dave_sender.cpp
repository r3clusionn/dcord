// dave_sender: plays the voice server's MLS external sender for tests.
// Line protocol on stdin/stdout, binary values as hex:
//   sender                      -> marshalled external sender
//   propose EPOCH KEYPACKAGE    -> proposals message (op 27 payload)
//   split COMMITWELCOME         -> "COMMIT WELCOME" (op 29 / op 30 payloads)
#include <iostream>
#include <sstream>
#include <string>

#include "external_sender.h"

static std::string hex(const std::vector<uint8_t> &v)
{
	static const char *d = "0123456789abcdef";
	std::string s;
	for (uint8_t b : v) {
		s += d[b >> 4];
		s += d[b & 15];
	}
	return s;
}

static std::vector<uint8_t> unhex(const std::string &s)
{
	std::vector<uint8_t> v;
	for (size_t i = 0; i + 1 < s.size(); i += 2)
		v.push_back((uint8_t)std::stoi(s.substr(i, 2), nullptr, 16));
	return v;
}

int main(int argc, char **argv)
{
	uint64_t group = argc > 1 ? std::stoull(argv[1]) : 1;
	discord::dave::test::ExternalSender es(1, group);
	std::string line;
	while (std::getline(std::cin, line)) {
		std::istringstream in(line);
		std::string cmd;
		in >> cmd;
		try {
			if (cmd == "sender") {
				std::cout << hex(es.GetMarshalledExternalSender()) << std::endl;
			} else if (cmd == "propose") {
				uint32_t epoch;
				std::string kp;
				in >> epoch >> kp;
				std::cout << hex(es.ProposeAdd(epoch, unhex(kp))) << std::endl;
			} else if (cmd == "split") {
				std::string cw;
				in >> cw;
				auto [c, w] = es.SplitCommitWelcome(unhex(cw));
				std::cout << hex(c) << " " << hex(w) << std::endl;
			} else {
				std::cout << "ERR unknown" << std::endl;
			}
		} catch (const std::exception &e) {
			std::cout << "ERR " << e.what() << std::endl;
		}
	}
}
