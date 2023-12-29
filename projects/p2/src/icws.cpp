#include <sys/types.h>
#include <string.h>
#include <sys/socket.h>
//#include <winsock.h>
#include <netdb.h>
#include <stdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <getopt.h>
#include <time.h>
#include <stdbool.h>
#include <poll.h>
#include <signal.h>
#include <netinet/in.h>
#include "simple_work_queue.hpp"
extern "C"
{
#include "parse.h"
#include "pcsa_net.h"
}

#define BUFSIZE 8192

//*DISCLAIMER*
// This project was done up until MILESTONE 2 only, MILESTONE 3 has not been implemented
// However the submission is benmarked according to MILESTONE 3's requirements

// MILESTONE 3
// Compiled references
// For strstr to find carriage return
// https://stackoverflow.com/questions/19971164/c-trying-to-find-r-n-r-n-in-headers
// For getting file extension
// https://stackoverflow.com/questions/5309471/getting-file-extension-in-c
// For HTTP date formatting
// https://stackoverflow.com/questions/7548759/generate-a-date-string-in-http-response-date-format-in-c
// For conditional mutex in threadpool
// https://stackoverflow.com/questions/6954489/how-to-utilize-a-thread-pool-with-pthreads
// For longopt
// https://azrael.digipen.edu/~mmead/www/Courses/CS180/getopt.html#LONGOPTS
// For poll
// https://man7.org/linux/man-pages/man2/poll.2.html
// For handling broken pipe
// https://stackoverflow.com/questions/18935446/program-received-signal-sigpipe-broken-pipe

// People who gave me suggestions:
// 1) Ajarn Kanat
// Suggestions: Everything (memcpy, pipelining, thread-safety, null terminators, etc.)
// 2) Pornkamol
// Suggestions: Calloc, general discussion about the project
// 3) Thanawin (Previous term) 
// Suggestions: Potential errors that I may encounter, memset() to clear out junk values
// 4) Phupha (Previous term)
// Suggestions: Expanding buffer for pipelined request storage, helping me understand yacc

typedef struct sockaddr SA;

struct
{
    work_queue workq;
} shared;

// Declare mutex as global variables
pthread_mutex_t jobsMutex;
pthread_mutex_t parserMutex;
pthread_cond_t signaler;
// Default timeout
int pollTimeout = 400;



char *determineMIME(char *extension)
{
    if (strcmp(extension, "jpg") == 0)
    {
        return "image/jpg";
    }
    else if (strcmp(extension, "png") == 0)
    {
        return "image/png";
    }
    else if (strcmp(extension, "gif") == 0)
    {
        return "image/gif";
    }
    else if (strcmp(extension, "html") == 0)
    {
        return "text/html";
    }
    else if (strcmp(extension, "css") == 0)
    {
        return "text/css";
    }
    else if (strcmp(extension, "plain") == 0)
    {
        return "text/plain";
    }
    else if (strcmp(extension, "javascript") == 0)
    {
        return "text/javascript";
    }
    else
    {
        return "unsupported";
    }
}

struct tm *getDateNow()
{
    time_t currentTime = time(0);
    struct tm *dateResultPointer = (tm *)malloc(sizeof(struct tm));

    // gmtime_r is threadsafe
    return gmtime_r(&currentTime, dateResultPointer);
}

char *findExtension(char *finalDir)
{
    // ref: https://stackoverflow.com/questions/5309471/getting-file-extension-in-c
    char *tempPointer = finalDir;
    char *extensionPointer = strrchr(tempPointer, '.');
    if (extensionPointer == tempPointer || !extensionPointer)
    {
        return "";
    }
    else
    {
        return extensionPointer + 1;
    }
}

void respondError(int connFd, char *errorCode, char *connectionState)
{
    // get current date
    char errorDateBuf[1000];
    struct tm *errorCurrentDate = getDateNow();
    strftime(errorDateBuf, sizeof errorDateBuf, "%a, %d %b %Y %H:%M:%S %Z", &(*errorCurrentDate));

    char errorResponseBuf[BUFSIZE];
    memset(errorResponseBuf, 0, BUFSIZE);

    sprintf(errorResponseBuf,
            "HTTP/1.1 %s\r\n"
            "Date: %s\r\n"
            "Server: icws\r\n"
            "Connection: %s\r\n\r\n",
            errorCode, errorDateBuf, connectionState);

    write_all(connFd, errorResponseBuf, strlen(errorResponseBuf));
    free(errorCurrentDate);
}

void respond(int connFd, char *finalDir, bool headerFlag, char *connectionState)
{
    //-----Get current date------
    char dateBuf[1000];
    struct tm *currentDate = getDateNow();
    strftime(dateBuf, sizeof dateBuf, "%a, %d %b %Y %H:%M:%S %Z", &(*currentDate));
    //---------------------------

    //------Find MIME------
    char *fileExtension = findExtension(finalDir);
    char *mime = determineMIME(fileExtension);
    if (strcmp(mime, "unsupported") == 0)
    {
        respondError(connFd, "400 Bad Request", connectionState);
        return;
    }
    //---------------------------

    //------Open the file to send-----
    char fileBuf[BUFSIZE];
    memset(fileBuf, 0, BUFSIZE);
    char *fileBufPointer = fileBuf;
    int fileDescriptor = open(finalDir, O_RDONLY);

    // If file cannot be opened, send an error 404
    if (fileDescriptor < 0)
    {
        // send 404 error
        respondError(connFd, "404 Not Found", connectionState);
        close(fileDescriptor);
        return;
    }
    //-----------------------------

    //-----Generate and send the header-----
    // determinine file statistics
    struct stat fileStatBuf;
    stat(finalDir, &fileStatBuf);
    printf("dir: %s\n", finalDir);
    size_t fileLen = fileStatBuf.st_size;
    // ref https://stackoverflow.com/questions/7548759/generate-a-date-string-in-http-response-date-format-in-c
    char timeBuf[1000];
    struct tm timeResult;
    struct tm fileTime = *gmtime_r(&(fileStatBuf.st_mtime), &timeResult);
    strftime(timeBuf, sizeof timeBuf, "%a, %d %b %Y %H:%M:%S %Z", &fileTime);

    // sending the header
    char responseBuf[BUFSIZE];
    sprintf(responseBuf,
            "HTTP/1.1 200 OK\r\n"
            "Date: %s\r\n"
            "Server: icws\r\n"
            "Connection: %s\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %lu\r\n"
            "Last-Modified: %s\r\n\r\n",
            dateBuf, connectionState, mime, fileLen, timeBuf);

    write_all(connFd, responseBuf, strlen(responseBuf));
    free(currentDate);
    //----------------------------------

    //-----Sending the actual file------
    if (headerFlag != true)
    {
        int numToWrite = 99;
        // Loop to deal with shortcounts
        while (numToWrite > 0)
        {
            // loop write
            numToWrite = read(fileDescriptor, fileBufPointer, BUFSIZE);
            // Error reading
            if (read < 0)
            {
                fprintf(stderr, "Error reading\n");
                break;
            }

            // printf("read %d\n", numToWrite);
            int toWriteThisRound = numToWrite;
            if (numToWrite <= 0)
            {
                break;
            }

            // Deal with shortcount
            while (toWriteThisRound > 0)
            {
                // writes to the pipe
                int writtenThisRound = write(connFd, fileBufPointer, toWriteThisRound);

                // handle write error, break but dont send ERROR code
                if (writtenThisRound <= 0)
                {
                    fprintf(stderr, "ERROR writing\n");
                    break;
                }
                toWriteThisRound -= writtenThisRound;
                fileBufPointer += writtenThisRound;
            }
            // Reset pointer
            fileBufPointer = fileBuf;
        }
    }
    close(fileDescriptor);
    //----------------------------

    return;
}

void server(int connFd, char *directory)
{

    // -----Setup buffers, polls, and memset to prevent junk values-----
    char roundBuf[BUFSIZE];
    memset(roundBuf, 0, BUFSIZE);
    char *allRequestBuf = (char *)calloc(BUFSIZE, sizeof(char));
    char *requestPointer = allRequestBuf;
    char *copyBufPointer = allRequestBuf;
    bool firstRound = true;
    bool connectionHeaderFound = false;

    int currentSize = BUFSIZE;
    int totalRead = 0;
    int readThisRound = 0;

    char *carriage = "\r\n\r\n";
    char *connectionState = "keep-alive";

    struct pollfd poller[1];
    poller[0].fd = connFd;
    poller[0].events = POLLIN;
    //------------------------


    bool aliveFlag = true;
    while (aliveFlag == true)
    {
        // Keep reading from connection
        while (true)
        {
            if (aliveFlag == false)
            {
                break;
            }

            // Poll if read is ready to be read
            // Set default to error (-1), if everything is okay, then the poll should return positive
            // Overwriting this error, else it is an error
            int pollReady = -1;
            if(firstRound == false){
                pollReady = poll(poller, 1, pollTimeout);
            }
            else if(firstRound == true){
                // Always ready the first round without polling
                pollReady = 1;
            }

            if (pollReady == 0)
            {
                // Waited too long, send 408 timeout error
                respondError(connFd, "408 Timeout Error", connectionState);
                free(allRequestBuf);
                return;
            }
            else if (pollReady < 0)
            {
                // Connection is not ready, send 400 bad request
                respondError(connFd, "400 Bad Request", connectionState);
                free(allRequestBuf);
                return;
            }
            else
            {
                firstRound = false;
                // printf("allrequest buffer\n%s\n",allRequestBuf);

                //-----------Parse Header-----------
                
                //Read invalid unless proven otherwise
                int readThisRound = -1;
                readThisRound = read(connFd, roundBuf, BUFSIZE);
                if (readThisRound > 0)
                {

                    totalRead += readThisRound;
                    if (totalRead >= currentSize)
                    {
                        // Must +1 or else segfault
                        // Expand memory to store more requests
                        currentSize = totalRead + 1;
                        allRequestBuf = (char *)realloc(allRequestBuf, sizeof(char) * currentSize);
                    }

                    //Memcpy to keep null terminator
                    memcpy(copyBufPointer,roundBuf,readThisRound);
                    copyBufPointer+=readThisRound;

                     //Check if there is a carriage return in the request
                    char *finalCarriageOccurence = strstr(allRequestBuf, carriage);
                   
                    //If none is found, then loop back to read again to get more data to form a request
                    //If the carriage is found, then there is a request, parse it immediately
                    if (!(finalCarriageOccurence == NULL))
                    {
                        // Carriage return found, there is a request here
                        // There can be more than 1 request in a read
                        // While there is a next request, parse and process it right away for pipelining
                        while (requestPointer != NULL)
                        {
                            //  if there is a complete request then parse
                            pthread_mutex_lock(&parserMutex);
                            Request *request = parse(requestPointer, currentSize, connFd, currentSize/500);
                            pthread_mutex_unlock(&parserMutex);

                            if (request == NULL)
                            {
                                // Because request headers can be empty, send an error
                                respondError(connFd, "400 Bad Request", connectionState);
                                free(request);
                                break;
                            }
                            //------------------------------------

                            //-----Print parsed HTTP request details-------
                            printf("------Request Details-----\n");
                            char *method = request->http_method;
                            printf("Http method: %s\n", method);
                            char *version = request->http_version;
                            printf("Http Version: %s\n", version);
                            char *uri = request->http_uri;
                            printf("Http Uri %s\n", uri);
                            char *contentLength = NULL;

                            printf("------Request Header------\n");
                            for (int index = 0; index < request->header_count; index++)
                            {
                                if(strcmp(request->headers[index].header_name, "Connection") == 0){
                                    //If there is connection header in the request, mark as true
                                    connectionHeaderFound = true;
                                }

                                if ((strcmp(request->headers[index].header_name, "Connection") == 0) && (strcmp(request->headers[index].header_value, "close") == 0))
                                {
                                    // Check if connection is still alive
                                    connectionState = "close";
                                    aliveFlag = false;
                                }
                                if ((strcmp(request->headers[index].header_name, "Content-Length") == 0))
                                {
                                    // Content length
                                    contentLength = request->headers[index].header_value;
                                }
                                printf("Header name: %s | Header Value: %s\n", request->headers[index].header_name, request->headers[index].header_value);
                            }

                            //--------------------------------------------

                            if(connectionHeaderFound == false){
                                // No connection header found
                                // request is not defined as kept-alive, default to close
                                connectionState = "close";
                                aliveFlag = false;
                            }


                            //-----Build directory path------
                            // get uri to build directory path
                            char directoryArray[500];
                            strcpy(directoryArray, directory);
                            char *finalDir = strcat(directoryArray, uri);
                            //-------------------------------


                            //-----Check validity and respond-----
                            bool headerFlag = false;
                            // No handling PUT or POST, no need for Content Length
                            // if((aliveFlag==false) && (atoi(contentLength)>8192)){
                            //     // if content length > 8192 reject only if not persistent
                            //     respondError(connFd, "400 Bad Request", error);
                            //     free(request->headers);
                            //     free(request);
                            // }
                            if (strcmp(version, "HTTP/1.1") != 0)
                            {
                                // HTTP version is not 1.1, send 505 error
                                respondError(connFd, "505 HTTP Version Not Supported", connectionState);
                                free(request->headers);
                                free(request);
                            }
                            else if (strcmp(method, "GET") == 0)
                            {
                                // There is a request of the form GET / {SOME_VERSION}
                                // Respond normally
                                respond(connFd, finalDir, headerFlag, connectionState);
                                free(request->headers);
                                free(request);
                            }
                            else if ((strcmp(method, "HEAD") == 0))
                            {

                                // Respond but without sending the body
                                headerFlag = true;
                                respond(connFd, finalDir, headerFlag, connectionState);
                                free(request->headers);
                                free(request);
                            }
                            else
                            {
                                // If not GET or HEAD, then it must be other request
                                // other requests aren't implemented
                                respondError(connFd, "501 Not Implemented", connectionState);
                                free(request->headers);
                                free(request);
                            }

                            // Advance pointer up until carriage return
                            requestPointer = strstr(requestPointer, carriage);
                            char *nextPointer = strstr(requestPointer + 4, carriage);

                            // If there is no next carriage return, there is no next request
                            if (nextPointer == NULL)
                            {
                                //Advanced the pointer anyway to process the next request when it is received after the loop
                                requestPointer += 4;
                                break;
                            }
                            // Advance to the next request
                            requestPointer += 4;
                        }
                    }
                    //Clear out junk values
                    memset(roundBuf, 0, BUFSIZE);
                }
                else
                {
                    // Read returns either 0 or an error
                    // No more information to be read, close the connection
                    // To prevent infinite looping
                    aliveFlag = false;
                    break;
                }
            }
        }
    }
    free(allRequestBuf);
    return;
    //-----------------------
}

void connHandler(void *args)
{
    struct survival_bag *context = (struct survival_bag *)args;

    // Service GET and HEAD
    // No need to destroy detached threads
    pthread_detach(pthread_self());
    server(context->connFd, context->directory);

    // Close the connection
    close(context->connFd);
}

// Threadpool
void *threadpool(void *args)
{
    for (;;)
    {
        // loop forever until told to quit
        // grab a job and work on it
        // ref: https://stackoverflow.com/questions/6954489/how-to-utilize-a-thread-pool-with-pthreads

        struct survival_bag *threadStorage;
        pthread_mutex_lock(&jobsMutex);

        // If there are no more jobs, the threads must wait
        while (shared.workq.isEmpty())
        {
            pthread_cond_wait(&signaler, &jobsMutex);
        }

        shared.workq.remove_job(&threadStorage);
        pthread_mutex_unlock(&jobsMutex);

        // Process the thread
        connHandler(threadStorage);
        free(threadStorage);
    }
}

int main(int argc, char *argv[])
{
    // Prevents crashing
    signal(SIGPIPE, SIG_IGN);

    // Initialize basic variables
    int c;
    int optionCount = 0;
    char *listenPort;
    char *wwwDirectory;

    // Default threadCount
    int threadCount = 4;

    // Initialize mutexes
    pthread_mutex_init(&jobsMutex, NULL);
    pthread_mutex_init(&parserMutex, NULL);
    pthread_cond_init(&signaler, NULL);


    // -----PARSE COMMAND OPTIONS-----
    // ref: https://azrael.digipen.edu/~mmead/www/Courses/CS180/getopt.html#LONGOPTS
    while (1)
    {
        int optionIndex = 0;
        static struct option longOptions[] =
            {
                {"port", required_argument, NULL, 0},
                {"root", required_argument, NULL, 0},
                {"numThreads", required_argument, NULL, 0},
                {"timeout", required_argument, NULL, 0},
                {NULL, 0, NULL, 0}};
        c = getopt_long(argc, argv, "a:b:c:d:", longOptions, &optionIndex);
        if (c == -1){
            break;
        }
        switch (c)
        {
        case 0:
            // printf("long option %s", longOptions[optionIndex].name);
            if (optarg){
                // printf(" with arg %s", optarg);
            }
            if (strcmp(longOptions[optionIndex].name, "port") == 0)
            {
                listenPort = optarg;
            }
            else if (strcmp(longOptions[optionIndex].name, "root") == 0)
            {
                wwwDirectory = optarg;
            }
            else if (strcmp(longOptions[optionIndex].name, "numThreads") == 0)
            {
                threadCount = atoi(optarg);
            }
            else if (strcmp(longOptions[optionIndex].name, "timeout") == 0)
            {
                // Convert to milliseconds
                pollTimeout = atoi(optarg) * 1000;
            }
            // printf("\n");
            optionCount++;
            break;
        default:
            printf("Incomplete arguments\n");
            return 0;
        }
    }
    // -----------------------


    // -----Initialize threads-----
    pthread_t activeThreads[threadCount];
    for (int a = 0; a < threadCount; a++)
    {
        pthread_create(&(activeThreads[a]), NULL, &threadpool, NULL);
    }
    //--------------------------


    //----------Main program----------
    if (optionCount >= 2)
    {
        int listenFd = open_listenfd(listenPort);

        for (;;)
        {
            struct sockaddr_storage clientAddr;
            socklen_t clientLen = sizeof(struct sockaddr_storage);

            int connFd = accept(listenFd, (SA *)&clientAddr, &clientLen);

            // skip round until a valid file descriptor is received
            if (connFd < 0)
            {
                continue;
            }

            char hostBuf[BUFSIZE], svcBuf[BUFSIZE];
            if (getnameinfo((SA *)&clientAddr, clientLen, hostBuf, BUFSIZE, svcBuf, BUFSIZE, 0) == 0)
            {
                printf("Connection from %s:%s\n", hostBuf, svcBuf);
            }
            else
            {
                printf("Connection from UNKNOWN.");
            }

            // Allocate space for context
            struct survival_bag *context =
                (struct survival_bag *)malloc(sizeof(struct survival_bag));

            // Put all job variables inside context
            context->connFd = connFd;
            context->directory = wwwDirectory;
            memcpy(&context->clientAddr, &clientAddr, sizeof(struct sockaddr_storage));

            // Add context to queue
            // Signal when add is completed
            pthread_mutex_lock(&jobsMutex);
            shared.workq.add_job(context);
            pthread_mutex_unlock(&jobsMutex);
            pthread_cond_signal(&signaler);
        }
    }
    else
    {
        printf("Invalid arguments\n");
        return 0;
    }

    //  Destroy mutexes
    pthread_mutex_destroy(&jobsMutex);
    pthread_mutex_destroy(&parserMutex);
    pthread_cond_destroy(&signaler);

    printf("Server ended\n");
    return 0;
    //------------------------------
}