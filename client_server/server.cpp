//
// Created by julia on 16/11/2020.
//

#undef UNICODE

#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdlib.h>
#include <stdio.h>
#include <tlhelp32.h>
#include <tchar.h>
#include <iostream>
#include <processthreadsapi.h>
#include <stdlib.h>
#include <algorithm>
#include <fstream>
#include <netinet/tcp.h>
#include "./cxxopts/include/cxxopts.hpp"

// Need to link with Ws2_32.lib
#pragma comment (lib, "Ws2_32.lib")
// #pragma comment (lib, "Mswsock.lib")

#define START_PTD 100
#define STOP_PTD 200
#define GET_DATA 500

#define DEFAULT_BUFFER_CHUNK_SIZE 4096
#define DEFAULT_FILE_CHUNK_SIZE 65536
#define DEFAULT_BUFLEN 512

#define LOG_FILE_PATH "D:\\work\\c\\logs_ptdeamon.txt"
#define PTD_COMMAND "D:\\work\\spec_ptd-main\\PTD\\ptd-windows-x86.exe -l logs_ptdeamon.txt -e  -p 8888 -y 49 C2PH13047V -V 'a'\\n\""
#define SCRIPT_COMMAND "perl D:\\work\\spec_ptd-main\\ptd-test.pl"
#define NTPD_START "w32tm.exe /resync"

struct serverAnswer {
    int err_code;
    char err_msg[DEFAULT_BUFLEN];
};

int sentMessage(int ClientSocket, char * message, size_t msgLength) {
    int iSendResult = send(ClientSocket, message, msgLength, 0);
    if (iSendResult == SOCKET_ERROR) {
        std::cerr << "Send failed with error: " << WSAGetLastError() << "Exit (1)" << std::endl;
        closesocket(ClientSocket);
        WSACleanup();
        return 1;
    }

    return 0;
}

int sentAnswerForClient(int ClientSocket, serverAnswer *answer) {
    sentMessage(ClientSocket, (char *)answer, sizeof(serverAnswer));
    std::cout << "Send message to client: code is " << answer->err_code << ", " << "message is " << answer->err_msg << std::endl;
    return 0;
}

int sentAnswer(int ClientSocket, int code, char *msg) {
    serverAnswer answer;
    answer.err_code = code;
    sprintf(answer.err_msg, msg);
    return sentAnswerForClient(ClientSocket, &answer);
}

int64_t GetFileSize(const std::string &fileName) {
    FILE *f;
    if ((f = fopen(fileName.c_str(), "rb")) == NULL) {
        return -1;
    }
    fseek(f, 0, SEEK_END);
    const int64_t len = ftell(f);
    fclose(f);
    return len;
}

int SendBuffer(SOCKET s, const char *buffer, int bufferSize, int chunkSize = DEFAULT_BUFFER_CHUNK_SIZE) {
    int allSendedBytes = 0;
    while (allSendedBytes < bufferSize) {
        const int sendedBytes = send(s, &buffer[allSendedBytes], std::min(chunkSize, bufferSize - allSendedBytes), 0);
        if (sendedBytes < 0) {
            return sendedBytes;
        }
        allSendedBytes += sendedBytes;
    }
    return allSendedBytes;
}

int64_t SendFile(SOCKET s, const std::string &fileName, int chunkSize = DEFAULT_FILE_CHUNK_SIZE) {
    const int64_t fileSize = GetFileSize(fileName);
    if (fileSize < 0) {
        std::cerr << "Can not get file size" << std::endl;
        return 1;
    }

    std::ifstream file(fileName, std::ifstream::binary);
    if (file.fail()) {
        std::cerr << "Can not open ifstream" << std::endl;
        return 1;
    }

    // Send file size
    if (SendBuffer(s, reinterpret_cast<const char *>(&fileSize),
                   sizeof(fileSize)) != sizeof(fileSize)) {
        std::cerr << "Can not send file size" << std::endl;
        return 1;
    }

    char *buffer = new char[chunkSize];
    bool errored = false;
    int64_t i = fileSize;
    while (i != 0) {
        const int64_t ssize = std::min(i, (int64_t) chunkSize);
        if (!file.read(buffer, ssize)) {
            errored = true;
            break;
        }
        const int l = SendBuffer(s, buffer, (int) ssize);
        if (l < 0) {
            errored = true;
            break;
        }
        i -= l;
    }
    delete[] buffer;
    if (!errored) {
        std::cout << "Send file to client:" << LOG_FILE_PATH << std::endl;
    } else {
        std::cerr << "Can not send file to client:" << LOG_FILE_PATH << std::endl;
    }

    file.close();

    return errored ? 1 : fileSize;
}

bool executeSystemCommand(char *command_Line, STARTUPINFO *si, PROCESS_INFORMATION *pi) {
    ZeroMemory(si, sizeof(*si));
    si->cb = sizeof(si);
    ZeroMemory(pi, sizeof(*pi));

    // Start the child process.
    if (!CreateProcess(NULL,   // No module name (use command line)
                       command_Line,        // Command line
                       NULL,           // Process handle not inheritable
                       NULL,           // Thread handle not inheritable
                       FALSE,          // Set handle inheritance to FALSE
                       CREATE_NEW_CONSOLE, // No creation flags
                       NULL,           // Use parent's environment block
                       NULL,           // Use parent's starting directory
                       si,            // Pointer to STARTUPINFO structure
                       pi)           // Pointer to PROCESS_INFORMATION structure
            ) {
        std::cerr << "CreateProcess failed " << GetLastError() << "Exit (1)" << std::endl;
        return (false);
    }
    return (true);
}

bool closeSystemProcess(PROCESS_INFORMATION *pi) {
    return TerminateProcess(pi->hProcess, 0) && CloseHandle(pi->hProcess) && CloseHandle(pi->hThread);
}

int startPtdClient() {
    WSADATA wsaData;
    SOCKET ConnectSocket = INVALID_SOCKET;
    struct addrinfo *result = NULL,
            hints, ServerAddress;
    int iResult;

    // Initialize Winsock
    iResult = WSAStartup(MAKEWORD(2,2), &wsaData);
    if (iResult != 0) {
        printf("WSAStartup failed with error: %d\n", iResult);
        return -1;
    }

    ZeroMemory( &hints, sizeof(hints) );
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    // Resolve the server address and port
    iResult = getaddrinfo(PTD_IP, PTD_PORT, &hints, &result);
    if (iResult != 0) {
        printf("getaddrinfo failed with error: %d\n", iResult);
        WSACleanup();
        return -1 ;
    }
    // Attempt to connect to an address until one succeeds
    // Create a SOCKET for connecting to server
    ConnectSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (ConnectSocket == INVALID_SOCKET) {
        printf("socket failed with error: %ld\n", WSAGetLastError());
        WSACleanup();
        return -1;
    }

    // Connect to PTD.
    sockaddr_in clientService;
    clientService.sin_family = AF_INET;
    clientService.sin_addr.s_addr = inet_addr("127.0.0.1");
    clientService.sin_port = htons(8888);

    int i = 0;
    while (i < 30) {
        iResult = connect(ConnectSocket, (SOCKADDR *) & clientService, sizeof (clientService));
        if (iResult != SOCKET_ERROR){
            break;
        }
        i++;
        Sleep(1000);
    }

    if (iResult == SOCKET_ERROR) {
        closesocket(ConnectSocket);
        ConnectSocket = INVALID_SOCKET;
    }

    freeaddrinfo(result);

    if (ConnectSocket == INVALID_SOCKET) {
        printf("Unable to connect to PTD!\n");
        WSACleanup();
        return -1 ;
    }

    std::cout << "Ready to send messages to ptd" << std::endl;

    const char sendBuf[] = "Identify\r\n";
    iResult = send(ConnectSocket, sendBuf, (int)strlen(sendBuf), 0 );
    if (iResult == SOCKET_ERROR) {
        printf("send failed with error: %d\n", WSAGetLastError());
        closesocket(ConnectSocket);
        WSACleanup();
        return -1;
    }

    std::cout << "Send message to ptd" << std::endl;

    char recvbuf[DEFAULT_BUFLEN];
    iResult = recv(ConnectSocket, recvbuf, DEFAULT_BUFLEN, 0);
    if ( iResult > 0 )
        printf("Bytes received: %d\n", iResult);
    else if ( iResult == 0 )
        printf("Connection closed\n");
    else
        printf("recv failed with error: %d\n", WSAGetLastError());

    return ConnectSocket;

}

void recvPtdAnswer(int ptdClientSocket) {
    char recvbuf[DEFAULT_BUFLEN];
    int iResult = recv(ptdClientSocket, recvbuf, DEFAULT_BUFLEN, 0);
    if ( iResult > 0 )
    {
        recvbuf[iResult] = '\0';
        std::cout << "PTD answer: " << recvbuf << std::endl;
    }
    else if ( iResult == 0 )
        std::cout << "Connection closed" << std::endl;
    else
        std::cout << "\"recv failed with error:" << WSAGetLastError() << std::endl;
}

struct InitMessage {
    int messageNumber;
    float averegeFloat;
};

int __cdecl main(int argc, char const *argv[]) {
    cxxopts::Options options("Server for communication with PTD", "A brief description");

    options.add_options()
            ("p,serverPort", "Server port", cxxopts::value<std::string>()->default_value("4950"))
            ("i,ipAddress", "Server ip address", cxxopts::value<std::string>())
            ("c,ptdConfigurationFile", "PTD configuration file path", cxxopts::value<std::string>()->default_value("config.txt"))
            ("h,help", "Print usage")
            ;

    auto parserResult = options.parse(argc, argv);

    if (parserResult.count("help"))
    {
        std::cout << options.help() << std::endl;
        exit(0);
    }

    std::string serverIpAddress;
    if (parserResult.count("ipAddress")) {
        serverIpAddress = parserResult["ipAddress"].as<std::string>();
    } else {
        std::cout << "Server ip address is required" << std::endl;
        return 1;
    }

    std::string serverPort = parserResult["serverPort"].as<std::string>();
    std::string configurationFile = parserResult["ptdConfigurationFile"].as<std::string>();

    WSADATA wsaData;
    int iResult;

    SOCKET ListenSocket = INVALID_SOCKET;
    SOCKET ClientSocket = INVALID_SOCKET;

    struct addrinfo *result = NULL;
    struct addrinfo hints;

    char recvbuf[DEFAULT_BUFLEN];
    int recvbuflen = DEFAULT_BUFLEN;

    char ptdCommand[] = PTD_COMMAND;
    char scriptCommand[] = SCRIPT_COMMAND;

    // Initialize Winsock
    iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (iResult != 0) {
        std::cerr << "WSAStartup failed with error: " << iResult << "Exit (1)" << std::endl;
        return 1;
    }
    STARTUPINFO siNtp;
    PROCESS_INFORMATION piNtp;
    executeSystemCommand(NTPD_START, &siNtp, &piNtp);

    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    // Resolve the server address and port
    iResult = getaddrinfo(serverIpAddress.c_str(), serverPort.c_str(), &hints, &result);
    if (iResult != 0) {
        std::cerr << "getaddrinfo failed with error: " << iResult << "Exit (1)" << std::endl;
        WSACleanup();
        return 1;
    }

    // Create a SOCKET for connecting to server
    ListenSocket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (ListenSocket == INVALID_SOCKET) {
        std::cerr << "getaddrinfo failed with error: " << WSAGetLastError() << "Exit (1)" << std::endl;
        freeaddrinfo(result);
        WSACleanup();
        return 1;
    }

    // Setup the TCP listening socket
    iResult = bind(ListenSocket, result->ai_addr, (int) result->ai_addrlen);
    if (iResult == SOCKET_ERROR) {
        std::cerr << "bind failed with error: " << WSAGetLastError() << "Exit (1)" << std::endl;
        freeaddrinfo(result);
        closesocket(ListenSocket);
        WSACleanup();
        return 1;
    }

    freeaddrinfo(result);

    // Accept a client socket
    iResult = listen(ListenSocket, 1);
    if (iResult == SOCKET_ERROR) {
        std::cerr << "listen failed with error: " << WSAGetLastError() << "Exit (1)" << std::endl;
        closesocket(ListenSocket);
        WSACleanup();
        return 1;
    }
    int ptdClientSocket;

    while (true) {
        DeleteFile(LOG_FILE_PATH);

        ClientSocket = accept(ListenSocket, NULL, NULL);
        if (ClientSocket == INVALID_SOCKET) {
            std::cerr << "accept failed with error: " << WSAGetLastError() << "Exit (1)" << std::endl;
            closesocket(ListenSocket);
            WSACleanup();
            return 1;
        }

        STARTUPINFO siPtd;
        PROCESS_INFORMATION piPtd;

        iResult = recv(ClientSocket, recvbuf, recvbuflen, 0);
        if (iResult > 0) {

            InitMessage * initMessage = (InitMessage *) recvbuf;
            std::cout << "Client command: " << initMessage->messageNumber << std::endl;
            bool is_ptd_started;
            char ptdCommand[] = PTD_COMMAND;
            is_ptd_started = executeSystemCommand(ptdCommand, &siPtd, &piPtd);
            if (!is_ptd_started) {
                sentAnswer(ClientSocket, 1, "Can not start process daemon");
            }
            ptdClientSocket = startPtdClient();

            if (ptdClientSocket < 0) {
                sentAnswer(ClientSocket, 1, "Can not open client socket for PTD");
            }
            if(initMessage->messageNumber == 100) {
                sentMessage(ptdClientSocket, "SR,A,Auto\r\n", strlen( "SR,A,Auto\r\n"));
                recvPtdAnswer(ptdClientSocket);

                sentMessage(ptdClientSocket, "SR,V,300\r\n", strlen( "SR,V,300\r\n"));
                recvPtdAnswer(ptdClientSocket);

                sentMessage(ptdClientSocket, "Go,1000,0\r\n", strlen( "Go,1000,0\r\n"));
                recvPtdAnswer(ptdClientSocket);
            } else {
                char buffer[DEFAULT_BUFLEN];
                std::cout << initMessage->averegeFloat << std::endl;
                sprintf(buffer, "SR,A,%f\r\n", initMessage->averegeFloat);
                std::cout << buffer << std::endl;
                sentMessage(ptdClientSocket, buffer, strlen(buffer));
                recvPtdAnswer(ptdClientSocket);
                sentMessage(ptdClientSocket, "Go,1000,0\r\n", strlen( "Go,1000,0\r\n"));
                recvPtdAnswer(ptdClientSocket);
            }
            if (is_ptd_started && (ptdClientSocket > -1)) {
                sentAnswer(ClientSocket, 0, "Start all needed processes");
            }
        }

        iResult = recv(ClientSocket, recvbuf, recvbuflen, 0);
        if (iResult > 0) {
            recvbuf[iResult] = '\0';
            bool IsPtdDeamonClosed;
            std::cout << "Client command: " << recvbuf << std::endl;
            sentMessage(ptdClientSocket, "Stop\r\n", strlen("Stop\r\n"));
            closesocket(ptdClientSocket);
            WSACleanup();
            IsPtdDeamonClosed = closeSystemProcess(&piPtd);
            if (!IsPtdDeamonClosed) {
                sentAnswer(ClientSocket, 1, "Can not stop process daemon");
            } else {
                sentAnswer(ClientSocket, 0, "Stop ptd.daemon and test.pl");
            }
        }

        iResult = recv(ClientSocket, recvbuf, recvbuflen, 0);
        if (iResult > 0) {
            recvbuf[iResult] = '\0';
            std::cout << "Client command: " << recvbuf << std::endl;
            SendFile(ClientSocket, LOG_FILE_PATH);
        }
    }

    iResult = shutdown(ClientSocket, SD_SEND);
    if (iResult == SOCKET_ERROR) {
        std::cerr << "shutdown failed with error: " << WSAGetLastError() << "Exit (1)" << std::endl;
        closesocket(ClientSocket);
        WSACleanup();
        return 1;
    }

    // cleanup
    closesocket(ClientSocket);
    WSACleanup();

    return 0;
}