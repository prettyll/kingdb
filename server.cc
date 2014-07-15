// Copyright (c) 2014, Emmanuel Goossaert. All rights reserved.
// Use of this source code is governed by the BSD 3-Clause License,
// that can be found in the LICENSE file.

#include "server.h"

namespace kdb {

void NetworkTask::Run(std::thread::id tid) {

  int bytes_received_last;
  std::regex regex_get {"get ([^\\s]*)"};
  std::regex regex_put {"set ([^\\s]*) \\d* \\d* (\\d*)\r\n"};

  uint32_t bytes_received_buffer = 0;
  uint32_t bytes_received_total  = 0;
  uint32_t bytes_expected = 0;
  uint64_t size_value = 0;
  uint64_t offset_value = 0;
  bool is_new = true;
  bool is_new_buffer = true;
  bool is_command_get = false;
  bool is_command_put = false;
  char *buffer = nullptr;
  char *buffer_get = new char[SIZE_BUFFER_SEND];
  char *key = nullptr;
  int size_key = 0;
  LOG_TRACE("NetworkTask", "ENTER");
  // TODO: replace the memory allocation performed for 'key' and 'buffer' by a
  //       pool of pre-allocated buffers

  while(true) {
    // Receive the data
    if (is_new) {
      LOG_TRACE("NetworkTask", "is_new");
      bytes_received_total = 0;
      bytes_expected = 0;
      size_value = 0;
      offset_value = 0;
      is_command_get = false;
      is_command_put = false;
      key = new char[1024]; // will be deleted by the storage engine
      size_key = 0;
    }

    if (is_new_buffer) {
      LOG_TRACE("NetworkTask", "is_new_buffer");
      bytes_received_buffer = 0;
      buffer = new char[SIZE_BUFFER_RECV+1]; // will be deleted by the storage engine
      LOG_TRACE("NetworkTask", "allocated");
    }

    bytes_received_last = recv(sockfd_,
                               buffer + bytes_received_buffer,
                               SIZE_BUFFER_RECV - bytes_received_buffer,
                               0);
    if (bytes_received_last <= 0) break;

    bytes_received_buffer += bytes_received_last;
    bytes_received_total  += bytes_received_last;
    buffer[bytes_received_buffer] = 0;

    LOG_TRACE("NetworkTask", "recv()'d %d bytes of data in buf - bytes_expected:%d bytes_received_buffer:%d bytes_received_total:%d", bytes_received_last, bytes_expected, bytes_received_buffer, bytes_received_total);

    if (is_new) {
      
      // Determine command type
      if (strncmp(buffer, "get", 3) == 0) {
        is_command_get = true;
      } else if (strncmp(buffer, "set", 3) == 0) {
        is_command_put = true;
      } else if (strncmp(buffer, "quit", 4) == 0) {
        break;
      }

      // Determine bytes_expected
      if (is_command_put) {
        offset_value = 0;
        while (buffer[offset_value] != '\n') offset_value++;
        offset_value++; // for the \n
        std::smatch matches;
        std::string str_buffer(buffer, offset_value);
        if (std::regex_search(str_buffer, matches, regex_put)) {
          size_key = matches[1].length();
          // TODO: check size of key before strncpy()
          strncpy(key, std::string(matches[1].str()).c_str(), size_key);
          key[size_key] = '\0';
          size_value = atoi(std::string(matches[2]).c_str());
          bytes_expected = offset_value + size_value + 2;
          // +2: because of the final \r\n
        } else {
          // should never happen, keeping it here until fully tested
          LOG_TRACE("NetworkTask", "Could not match put command [%s]", str_buffer.c_str());
          exit(-1);
        }
      } else if (   bytes_received_last >= 2
                 && buffer[bytes_received_last-2] == '\r'
                 && buffer[bytes_received_last-1] == '\n') {
        bytes_expected = bytes_received_last;
      } else {
        // should never happen, keeping it here until fully tested
        LOG_TRACE("NetworkTask", "Don't know what to do with this new packet [%s]", buffer);
        exit(-1);
      }
    }

    is_new = false;

    // Loop and get more data from the network if the buffer is not full and all the data
    // hasn't arrived yet
    if (   bytes_received_total < bytes_expected
        && bytes_received_buffer < SIZE_BUFFER_RECV) {
      // TODO: what if the \r\n is on the two last messages, i.e. \n is the
      // first character of the last message?
      is_new_buffer = false;
      continue;
    }

    if (is_command_get) {
      std::smatch matches;
      std::string str_buffer = buffer;
      if (std::regex_search(str_buffer, matches, regex_get)) {
        // ------ TEMP
        /*
        size_key = matches[1].length();
        strncpy(key, std::string(matches[1].str()).c_str(), size_key);
        key[size_key] = '\0';

        sprintf(buffer_get, "VALUE %s 0 %d\r\n%s\r\nEND\r\n", key, 5, "hello");
        if (send(sockfd_, buffer_get, strlen(buffer_get), 0) == -1) {
          LOG_TRACE("NetworkTask", "Error: send() - %s", strerror(errno));
          break;
        }
        LOG_TRACE("NetworkTask", "GET: buffer_get [%s]", buffer_get);
        is_new = true;
        is_new_buffer = true;
        delete[] buffer;
        continue;
        */
        // ------ TEMP

        Value *value; // beware, possible memory leak here
        Status s = db_->Get(matches[1], &value);
        if (s.IsOK()) {
          LOG_TRACE("NetworkTask", "GET: found");
          size_key = matches[1].length();
          // TODO: check size of key before strncpy()
          strncpy(key, std::string(matches[1].str()).c_str(), size_key);
          key[size_key] = '\0';
          //sprintf(buffer_get, "VALUE %s 0 %llu\r\n%s\r\nEND\r\n", key, value->size, value->data);
          std::string value_start(value->data, 2);
          sprintf(buffer_get, "VALUE %s 0 %llu\r\n", key, value->size);
          LOG_TRACE("NetworkTask", "GET: buffer_get [%s]", buffer_get);
          if (send(sockfd_, buffer_get, strlen(buffer_get), 0) == -1) {
            LOG_TRACE("NetworkTask", "Error: send() - %s", strerror(errno));
            break;
          }
          if (send(sockfd_, value->data, value->size, 0) == -1) {
            LOG_TRACE("NetworkTask", "Error: send() - %s", strerror(errno));
            break;
          }
          if (send(sockfd_, "\r\nEND\r\n", 7, 0) == -1) {
            LOG_TRACE("NetworkTask", "Error: send()", strerror(errno));
            break;
          }
        } else {
          LOG_TRACE("NetworkTask", "GET: [%s]", s.ToString().c_str());
          std::string msg = "NOT_FOUND\r\n";
          if (send(sockfd_, msg.c_str(), msg.length(), 0) == -1) {
            LOG_TRACE("NetworkTask", "Error: send() - %s", strerror(errno));
            break;
          }
        }
        delete value;
        is_new = true;
        is_new_buffer = true;
        delete[] buffer;
      }
    } else if (is_command_put) {
      char *chunk;
      uint64_t size_chunk;
      uint64_t offset_chunk;

      if(bytes_received_total == bytes_received_buffer) {
        // chunk is a first chunk, need to skip all the characters before the
        // value data
        chunk = buffer + offset_value;
        size_chunk = bytes_received_buffer - offset_value;
        offset_chunk = 0;
      } else {
        chunk = buffer;
        size_chunk = bytes_received_buffer;
        offset_chunk = bytes_received_total - bytes_received_buffer - offset_value;
      }

      if (bytes_received_total == bytes_expected) {
        // chunk is a last chunk
        // in case this is the last buffer, the size of the buffer needs to be
        // adjusted to ignore the final \r\n
        size_chunk -= 2;
      }

      //LOG_TRACE("NetworkTask", "buffer [%s]", buffer);
      if (size_chunk > 0) {
        LOG_TRACE("NetworkTask", "call PutChunk key [%s] bytes_received_buffer:%llu bytes_received_total:%llu bytes_expected:%llu size_chunk:%llu", key, bytes_received_buffer, bytes_received_total, bytes_expected, size_chunk);
        Status s = db_->PutChunk(key,
                                 size_key,
                                 chunk,
                                 size_chunk,
                                 offset_chunk,
                                 size_value,
                                 buffer);
        if (!s.IsOK()) {
          LOG_TRACE("NetworkTask", "Error - Put(): %s", s.ToString().c_str());
        } else {
          buffer = nullptr;
        }
      }

      if (bytes_received_total == bytes_expected) {
        is_new = true;
        LOG_TRACE("NetworkTask", "STORED key [%s] bytes_received_buffer:%llu bytes_received_total:%llu bytes_expected:%llu", key, bytes_received_buffer, bytes_received_total, bytes_expected);
        if (send(sockfd_, "STORED\r\n", 8, 0) == -1) {
          LOG_TRACE("NetworkTask", "Error - send() %s", strerror(errno));
          break;
        }

      }
      is_new_buffer = true;

    } else {
      // for debugging
      LOG_TRACE("NetworkTask", "Unknown case for buffer");
      exit(-1);
    }

    //if (bytes_received_total == bytes_expected) break;
  }
  LOG_TRACE("NetworkTask", "exit and close socket");

  if (buffer != nullptr) delete[] buffer;
  if (buffer_get != nullptr) delete[] buffer_get;
  close(sockfd_);
}


void* Server::GetSockaddrIn(struct sockaddr *sa)
{
  if (sa->sa_family == AF_INET) {
    return &(((struct sockaddr_in*)sa)->sin_addr);
  }
  return &(((struct sockaddr_in6*)sa)->sin6_addr);
}


Status Server::Start(std::string dbname, int port, int backlog, int num_threads) {

  // Ignoring SIGPIPE, which would crash the program when writing to
  // a broken socket -- doing this because MSG_NOSIGNAL doesn't work on Mac OS X
  signal(SIGPIPE, SIG_IGN);

  struct addrinfo ai_hints, *ai_server, *ai_ptr;
  memset(&ai_hints, 0, sizeof(ai_hints));
  ai_hints.ai_family = AF_UNSPEC;
  ai_hints.ai_socktype = SOCK_STREAM;
  ai_hints.ai_flags = AI_PASSIVE;
  std::string str_port = std::to_string(port);
  int ret;
  if ((ret = getaddrinfo(NULL, str_port.c_str(), &ai_hints, &ai_server)) != 0) {
    return Status::IOError("Server - getaddrinfo", gai_strerror(ret));
  }

  // Bind to the first result
  int sockfd_listen;
  for(ai_ptr = ai_server; ai_ptr != NULL; ai_ptr = ai_ptr->ai_next) {
    if ((sockfd_listen = socket(ai_ptr->ai_family, ai_ptr->ai_socktype, ai_ptr->ai_protocol)) == -1) {
      continue;
    }

    int setsockopt_yes=1;
    if (setsockopt(sockfd_listen, SOL_SOCKET, SO_REUSEADDR, &setsockopt_yes, sizeof(setsockopt_yes)) == -1) {
      close(sockfd_listen);
      return Status::IOError("Server - setsockopt", strerror(errno));
    }

    if (bind(sockfd_listen, ai_ptr->ai_addr, ai_ptr->ai_addrlen) == -1) {
      close(sockfd_listen);
      continue;
    }

    break;
  }

  if (ai_ptr == NULL) return Status::IOError("Server - Failed to bind");
  freeaddrinfo(ai_server);

  if (listen(sockfd_listen, backlog) == -1) {
    return Status::IOError("Server - listen", strerror(errno));
  }

  // Create the database object and the thread pool
  kdb::KingDB db(dbname);
  ThreadPool tp(num_threads);
  tp.Start();
  LOG_TRACE("Server", "waiting for connections...");

  // Start accepting connections
  int sockfd_accept;
  struct sockaddr_storage sockaddr_client;
  socklen_t size_sa;
  char address[INET6_ADDRSTRLEN];
  while(1) {
    size_sa = sizeof(sockaddr_client);
    sockfd_accept = accept(sockfd_listen, (struct sockaddr *)&sockaddr_client, &size_sa);
    if (sockfd_accept == -1) continue;

    inet_ntop(sockaddr_client.ss_family,
              GetSockaddrIn((struct sockaddr *)&sockaddr_client),
              address,
              sizeof(address));
    LOG_TRACE("Server", "got connection from %s\n", address);

    tp.AddTask(new NetworkTask(sockfd_accept, &db));
  }

  return Status::OK();
}

}
