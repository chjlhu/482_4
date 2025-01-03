#include <iostream>
#include <cassert>
#include <cstdlib>
#include <string>
#include "fs_client.h"
// AKA, whoops all errors

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
    // initialize stuff to call functions on
    status = fs_create("user1", "/dir", 'd');
    status = fs_create("user1", "/file1", 'f');
    status = fs_create("user1", "/dir/file2", 'f');
    assert(!status);
    // username too long
    status = fs_create("user1234567890", "/dir", 'd');
    assert(status != 0);
    // filename too long
    status = fs_create("user3", "/thisfilenameiswaytoolongthisfilenameiswaytoolongthisfilenameiswaytoolong", 'f');
    assert(status != 0);
    // make directory without parent
    status = fs_create("user3", "/dir2/file1", 'f');
    assert(status != 0);
    // delete non-empty directory
    status = fs_delete("user1", "/dir");
    assert(status != 0);
    // wrong owner
    status = fs_create("user2", "/dir/file3", 'f');
    assert(status != 0);
    status = fs_delete("user2", "/dir/file2");
    assert(status != 0);
}