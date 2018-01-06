#include "Base/Log.h"
#include "Base/Utils.h"

#include "BaseChannel.h"
#include "Host.h"
#include "TcpController.h"
#include "Utility/ThreadPool.h"

namespace {

using Executors::FixedThreadPool;
using net_stack::BaseChannel;
using net_stack::Host;
using net_stack::Process;
using std::placeholders::_1;

const char* const kAliceIP = "127.0.0.1";
const char* const kBobIP = "127.0.0.2";
const uint32 kAlicePort = 10;
const uint32 kBobPort = 20;

const uint32 kTestDataSize = 100;

byte* data;
byte* receive_buffer;

void InitData() {
  data = new byte[kTestDataSize];
  for (uint32 i = 0; i < kTestDataSize; i++) {
    data[i] = 'a';//Utils::RandomNumber(256);
  }

  receive_buffer = new byte[kTestDataSize];
}

bool ReceivedDataCorrect() {
  for (uint32 i = 0; i < kTestDataSize; i++) {
    if (receive_buffer[i] != data[i]) {
      printf("i = %d, '%c' and '%c'\n", i, receive_buffer[i], data[i]);
      return false;
    }
  }
  return true;
}

void Client(Process* process) {
  // Create client socket.
  int fd = process->Socket();
  if (fd < 0) {
    LogERROR("Failed to create client socket");
    return;
  }

  // Bind to static port.
  bool re = process->Bind(fd, kAliceIP, kAlicePort);
  if (!re) {
    LogERROR("Failed to bind socket %d to %s:%u", fd, kAliceIP, kAlicePort);
    return;
  }

  // Connect server.
  re = process->Connect(fd, kBobIP, kBobPort);
  if (!re) {
    LogERROR("Client Alice failed to connect to server Bob");
    return;
  }

  // Begin sending data to server.
  uint32 writen = 0;
  while (writen < kTestDataSize) {
    auto re = process->Write(fd, data + writen, kTestDataSize - writen);
    if (re > 0) {
      writen += re;
    } else {
      break;
    }
  }
  if (writen != kTestDataSize) {
    LogERROR("Alice sent %d bytes data\n", writen);
  }

  // Receive data from server, verify bytes are flipped.
  uint32 readn = 0;
  byte client_buffer[kTestDataSize];
  while (readn < kTestDataSize) {
    auto re = process->Read(fd, client_buffer + readn, kTestDataSize);
    if (re > 0) {
      readn += re;
    } else {
      break;
    }
  }
  if (readn != kTestDataSize) {
    LogERROR("Alice received %d bytes data", readn);
    process->Close(fd);
    return;
  }

  bool flip_correct = true;
  for (uint32 i = 0; i < kTestDataSize; i++) {
    if (client_buffer[i] != 256 - data[i]) {
      LogERROR("Alice received back wrong data!");
      flip_correct = false;
      break;
    }
  }
  if (flip_correct) {
    LogINFO("Alice received correct data \033[2;32m:)\033[0m");
  }

  process->Close(fd);
}

void Server(Process* process) {
  // Create server socket.
  int sock_fd = process->Socket();
  if (sock_fd < 0) {
    LogERROR("Failed to create server socket");
    return;
  }

  // Bind socket to listning port.
  bool re = process->Bind(sock_fd, kBobIP, kBobPort);
  if (!re) {
    LogERROR("Failed to bind socket %d to {%s, %u}",
             sock_fd, kBobIP, kBobPort);
    return;
  }

  // Listen on this socket.
  re = process->Listen(sock_fd);
  if (!re) {
    LogERROR("Failed to listen on socket %d", sock_fd);
    return;
  }

  // Server start accepting new connections.
  int tcp_socket = -1;
  while (true) {
    tcp_socket = process->Accept(sock_fd);
    if (tcp_socket < 0) {
      LogERROR("Accept failed");
      continue;
    }
    
    // Server handle request.
    uint32 readn = 0;
    while (readn < kTestDataSize) {
      auto re = process->Read(tcp_socket, receive_buffer + readn,
                              kTestDataSize);
      if (re > 0) {
        readn += re;
      }
      else if (re == 0) {
        LogINFO("Client closed socket, server now closing...");
        process->Close(tcp_socket);
        return;
      } else {
        break;
      }
      //printf("readn = %d\n", readn);
    }
    if (readn != kTestDataSize) {
      LogERROR("Bob received %d bytes data\n", readn);
      process->Close(tcp_socket);
      return;
    }
    if (!ReceivedDataCorrect()) {
      LogERROR("Receive data failed");
    } else {
      LogINFO("Bob received correct data \033[2;32m:)\033[0m");
    }

    // Service: Flip each byte and send back to client.
    for (uint32 i = 0; i < kTestDataSize; i++) {
      receive_buffer[i] = 256 - receive_buffer[i];
    }

    uint32 writen = 0;
    while (writen < kTestDataSize) {
      auto re = process->Write(tcp_socket, receive_buffer + writen,
                               kTestDataSize - writen);
      if (re > 0) {
        writen += re;
      }
    }
    if (writen != kTestDataSize) {
      LogERROR("Bob sent %d bytes data\n", writen);
    }
    process->Close(tcp_socket);
  }
}

}  // namespace

int main(int argc, char** argv) {
  // Channel is unidirectional. Define two channels.
  BaseChannel channel_alice_to_bob;
  BaseChannel channel_bob_to_alice;

  // Hello, Alice and Bob.
  Host alice("Alice", kAliceIP, &channel_alice_to_bob);
  Host bob("Bob", kBobIP, &channel_bob_to_alice);

  // Register receiver's callback to channel.
  channel_alice_to_bob.RegisterReceiverCallback(
      std::bind(&Host::MovePacketsFromChannel, &bob, _1));
  channel_bob_to_alice.RegisterReceiverCallback(
      std::bind(&Host::MovePacketsFromChannel, &alice, _1));

  channel_alice_to_bob.Start();
  channel_bob_to_alice.Start();

  // Create data to send.
  InitData();

  alice.CreateProcess("Client", Client);
  bob.CreateProcess("Server", Server);

  // Start hosts and run forever.
  alice.RunForever();
  bob.RunForever();
  while (true) {}
}
