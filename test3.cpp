#include <iostream>
#include <cassert>
#include <cstdlib>
#include <string.h>
#include "fs_client.h"
// read/write test

int main(int argc, char *argv[])
{
    char *server;
    int server_port;
    const char *writedata = "We hold these truths to be self-evident, that all men are created equal, that they are endowed by their Creator with certain unalienable Rights, that among these are Life, Liberty and the pursuit of Happiness. -- That to secure these rights, Governments are instituted among Men, deriving their just powers from the consent of the governed, -- That whenever any Form of Government becomes destructive of these ends, it is the Right of the People to alter or to abolish it, and to institute new Government, laying its foundation on such principles and organizing its powers in such form, as to them shall seem most likely to effect their Safety and Happiness.";
    char readdata[FS_BLOCKSIZE];
    int status;

    if (argc != 3)
    {
        std::cout << "error: usage: " << argv[0] << " <server> <serverPort> ";
        exit(1);
    }
    server = argv[1];
    server_port = atoi(argv[2]);
    fs_clientinit(server, server_port);
    status = fs_create("user1", "/file", 'f');
    assert(!status);
    uint32_t write_length = strlen(writedata);
    const uint32_t BLOCKSIZE = FS_BLOCKSIZE - 1;
    uint32_t block = 0;
    while ((block * BLOCKSIZE) < write_length)
    {
        const char *write = writedata + (block * BLOCKSIZE);
        status = fs_writeblock("user1", "/file", block, write);
        assert(!status);
        block++;
    }

    uint32_t at = 0;
    for (uint32_t i = 0; i < block; i++)
    {
        fs_readblock("user1", "/file", i, readdata);
        for (uint32_t j = 0; j < BLOCKSIZE; j++)
        {
            if (at > write_length)
            {
                fs_delete("user1", "/file");
                return 0;
            }
            if (readdata[j] != *(writedata + at))
            {
                printf("Failed at block %i, char %u\n", i, j);
                printf("%s\n", readdata);
                fs_delete("user1", "/file");
                return 0;
            }
            at++;
        }
    }
}