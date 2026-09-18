/* SPDX-License-Identifier: Apache-2.0 */
// A wire reference using this checkout's unmodified C++ WebSocket transport.
#include <thrift/transport/TWebSocketServer.h>
#include <thrift/transport/TServerSocket.h>
#include <thrift/protocol/TBinaryProtocol.h>
#include <thrift/protocol/TCompactProtocol.h>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
using namespace apache::thrift::transport;
using namespace apache::thrift::protocol;
namespace {
constexpr int kind_argument = 1;
constexpr int mux_argument = 2;
constexpr int argument_count = 3;
constexpr int rpc_iterations = 3;
constexpr int connection_timeout_ms = 5000;
}
int main(int argc, char **argv) {
  if (argc != argument_count) return EXIT_FAILURE;
  try {
    TServerSocket listener("127.0.0.1", 0);
    listener.setRecvTimeout(connection_timeout_ms);
    listener.setSendTimeout(connection_timeout_ms);
    listener.listen();
    std::cout << listener.getPort() << std::endl;
    auto socket = listener.accept();
    auto transport = std::make_shared<TWebSocketServer<true>>(socket);
    std::shared_ptr<TProtocol> protocol;
    if (std::string(argv[kind_argument]) == "compact")
      protocol = std::make_shared<TCompactProtocol>(transport);
    else
      protocol = std::make_shared<TBinaryProtocol>(transport);
    const std::string prefix = std::string(argv[mux_argument]) == "mux" ? "Echo:" : "";
    for (int iteration = 0; iteration < rpc_iterations; ++iteration) {
      for (const std::string method : {"notify", "inherited"}) {
        std::string name;
        TMessageType message_type;
        int32_t sequence, value;
        TType field_type;
        int16_t field_id;
        protocol->readMessageBegin(name, message_type, sequence);
        if (name != prefix + method || message_type != (method == "notify" ? T_ONEWAY : T_CALL))
          return EXIT_FAILURE;
        protocol->readStructBegin(name);
        protocol->readFieldBegin(name, field_type, field_id);
        if (field_type != T_I32 || field_id != 1) return EXIT_FAILURE;
        protocol->readI32(value);
        protocol->readFieldEnd();
        protocol->readFieldBegin(name, field_type, field_id);
        if (field_type != T_STOP) return EXIT_FAILURE;
        protocol->readStructEnd();
        protocol->readMessageEnd();
        if (method == "notify") {
          if (value != 7) return EXIT_FAILURE;
          continue;
        }
        if (value != 35) return EXIT_FAILURE;
        protocol->writeMessageBegin(method, T_REPLY, sequence);
        protocol->writeStructBegin("result");
        protocol->writeFieldBegin("success", T_I32, 0);
        protocol->writeI32(42);
        protocol->writeFieldEnd();
        protocol->writeFieldStop();
        protocol->writeStructEnd();
        protocol->writeMessageEnd();
        transport->flush();
      }
    }
    transport->close();
    listener.close();
    return EXIT_SUCCESS;
  } catch (const std::exception &error) {
    std::cerr << error.what() << std::endl;
    return EXIT_FAILURE;
  }
}
