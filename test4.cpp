#include <iostream>
#include <cassert>
#include <cstdlib>
#include <string>
#include "fs_client.h"
// stress test

void del(uint32_t files, bool exists)
{
    int status = 0;
    for (uint32_t i = 0; i < files; i++)
    {
        std::string path = "/filedir/file" + std::to_string(i);
        status = fs_delete("user1", path.c_str());
        assert(!status);
    }
    if (exists)
    {
        std::string path = "/filedir/file" + std::to_string(files);
        status = fs_delete("user1", path.c_str());
        assert(!status);
    }
}

int main(int argc, char *argv[])
{
    char *server;
    int server_port;
    const char *writedata = "We hold these truths to be self-evident, that all men are created equal, that they are endowed by their Creator with certain unalienable Rights, that among these are Life, Liberty and the pursuit of Happiness. -- That to secure these rights, Governments are instituted among Men, deriving their just powers from the consent of the governed, -- That whenever any Form of Government becomes destructive of these ends, it is the Right of the People to alter or to abolish it, and to institute new Government, laying its foundation on such principles and organizing its powers in such form, as to them shall seem most likely to effect their Safety and Happiness.";
    int status;

    if (argc != 3)
    {
        std::cout << "error: usage: " << argv[0] << " <server> <serverPort> ";
        exit(1);
    }
    server = argv[1];
    server_port = atoi(argv[2]);
    fs_clientinit(server, server_port);

    status = fs_create("user1", "/filedir", 'd');
    assert(!status);
    for (uint32_t i = 0; i < FS_MAXFILEBLOCKS; i++)
    {
        std::string path = "/filedir/file" + std::to_string(i);
        status = fs_create("user1", path.c_str(), 'f');
        if (status)
        {
            printf("Ending on file %s\n", path.c_str());
            del(i, false);
        }
        for (uint32_t j = 0; j < FS_MAXFILEBLOCKS; j++)
        {
            status = fs_writeblock("user1", path.c_str(), j, writedata);
            if (status)
            {
                printf("Ending on file %s and block %u\n", path.c_str(), j);
                del(i, true);
            }
        }
    }
    printf("Done testing!\n");
}