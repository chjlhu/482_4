#include <string>
#include <sstream>
#include <set>
#include <vector>
#include <unordered_map>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include "fs_server.h"

#define BUFFER_SIZE 1024
const std::string FS_READBLOCK = "FS_READBLOCK";
const std::string FS_WRITEBLOCK = "FS_WRITEBLOCK";
const std::string FS_CREATE = "FS_CREATE";
const std::string FS_DELETE = "FS_DELETE";
boost::shared_mutex mutex;

std::set<uint32_t> empty_blocks;
int get_write()
{
    if (empty_blocks.empty())
    {
        return -1;
    }
    return *empty_blocks.begin();
}

void write_helper(const char *out)
{
    cout_lock.lock();
    std::cout << out << std::endl;
    cout_lock.unlock();
}

enum DO
{
    READFILE,
    WRITEFILE,
    CREATEFILE,
    DELETEFILE
};

class Request
{
private:
    std::string command = "";
    std::string username = "";
    std::string pathname = "";
    std::string etc = "";
    char data[FS_BLOCKSIZE];

    void read(int at, void *ptr)
    {
        boost::shared_lock<boost::shared_mutex> lock(mutex);
        disk_readblock(at, ptr);
    }
    void write(int at, void *ptr)
    {
        boost::unique_lock<boost::shared_mutex> unique_lock(mutex);
        disk_writeblock(at, ptr);
        // remove block from set if not rewrite
        if (empty_blocks.find(at) != empty_blocks.end())
        {
            empty_blocks.erase(at);
        }
    }

    int search_root(int at, const char *cur)
    {
        // 1) load root directory
        fs_inode root;
        disk_readblock(at, (void *)&root);
        // 2) for block of direntries in root
        for (uint32_t i = 0; i < root.size; i++)
        {
            fs_direntry dirs[FS_DIRENTRIES];
            disk_readblock(root.blocks[i], (void *)&dirs);
            // 3) search each direntry
            for (uint32_t j = 0; j < FS_DIRENTRIES; j++)
            {
                // looks for dir
                if (dirs[j].inode_block == 0)
                {
                    return -1;
                }
                if (strcmp(dirs[j].name, cur) == 0)
                {
                    return dirs[j].inode_block;
                }
            }
        }
        return -1;
    }

    int find_dirblock(bool file)
    {
        size_t i = 1;
        int at = 0;
        std::string cur = "";
        fs_inode root;
        while (i < pathname.size())
        {
            if (pathname[i] == '/')
            {
                if (root.type == 'f')
                {
                    return -1;
                }
                at = search_root(at, cur.c_str());
                if (at < 0)
                {
                    return -1;
                }
                disk_readblock(at, (void *)&root);
                cur = "";
            }
            else
            {
                cur += pathname[i];
            }
            i++;
        }
        if (file)
        {
            // search to end
            at = search_root(at, cur.c_str());
        }
        return at;
    }

public:
    int receive(int sock)
    {
        // receive message
        char buffer[BUFFER_SIZE];
        auto buff_ptr = &buffer;
        int bytes_received = 0;
        int bytes = 0;
        do
        {
            bytes = recv(sock, buff_ptr, BUFFER_SIZE - 1, 0);
            buff_ptr += bytes;
            bytes_received += bytes;
        } while (bytes > 0);
        // bytes_received = recv(sock, buffer, BUFFER_SIZE - 1, 0);
        // command
        int i = 0;
        while (buffer[i] != ' ')
        {
            command += buffer[i];
            i++;
        }
        i++;
        // username
        while (buffer[i] != ' ')
        {
            username += buffer[i];
            i++;
        }
        i++;
        // directory
        //      delete
        if (command == FS_DELETE)
        {
            while (buffer[i] != '\0')
            {
                Request::pathname += buffer[i];
                i++;
            }
            return DO::DELETEFILE;
        }
        //      readblock, writeblock, create
        while (buffer[i] != ' ')
        {
            Request::pathname += buffer[i];
            i++;
        }
        i++;
        // etc...
        while (buffer[i] != '\0')
        {
            Request::etc += buffer[i];
            i++;
        }
        // writeblock data
        if (command == FS_WRITEBLOCK)
        {
            i++;
            int j = 0;
            while (i < bytes_received)
            {
                data[j] = buffer[i];
                i++;
                j++;
            }
            if (j == FS_BLOCKSIZE)
            {
                j--;
            }
            data[j] = '\0';
            return DO::WRITEFILE;
        }
        if (command == FS_READBLOCK)
        {
            return DO::READFILE;
        }
        if (command == FS_CREATE)
        {
            return DO::CREATEFILE;
        }
        return -1;
    }

    int readfile()
    {
        int at = find_dirblock(true);
        if (at < 0)
        {
            return -1;
        }

        int block = atoi(etc.c_str());
        fs_inode file;
        disk_readblock(at, (void *)&file);
        if (file.type != 'f')
        {
            return -1;
        }
        if (strcmp(file.owner, username.c_str()))
        {
            return -1;
        }
        if (block >= static_cast<int>(file.size))
        {
            return -1;
        }
        read(file.blocks[block], (void *)&data);
        return 0;
    }

    int writefile()
    {
        int at = find_dirblock(true);
        if (at < 0)
        {
            return -1;
        }

        uint32_t block = atoi(etc.c_str());
        fs_inode file;
        read(at, (void *)&file);
        if (file.type != 'f')
        {
            return -1;
        }
        if (strcmp(file.owner, username.c_str()))
        {
            return -1;
        }
        if (block == FS_MAXFILEBLOCKS)
        {
            return -1;
        }

        if (block < file.size)
        {
            write(file.blocks[block], (void *)&data);
            return 0;
        }
        if (block == file.size)
        {
            int wrt = get_write();
            if (wrt < 0)
            {
                return -1;
            }
            write(wrt, (void *)&data);
            file.blocks[file.size] = wrt;
            file.size = file.size + 1;
            write(at, (void *)&file);
            return 0;
        }
        return -1;
    }

    int create()
    {
        if (etc.length() > 1)
        {
            return -1;
        }
        if (etc[0] != 'd' && etc[0] != 'f')
        {
            return -1;
        }
        int at = find_dirblock(false);
        if (at < 0)
        {
            return -1;
        }

        fs_inode directory;
        read(at, (void *)&directory);
        if (directory.type != 'd')
        {
            return -1;
        }
        if (at > 0 && strcmp(directory.owner, username.c_str()))
        {
            return -1;
        }
        // 0) get file name
        size_t i = pathname.size();
        i--;
        while (pathname[i] != '/')
        {
            i--;
        }
        i++;
        std::string filename = pathname.substr(i, pathname.size() - i);
        if (filename.size() > FS_MAXFILENAME)
        {
            return -1;
        }
        // 1) make inode & write to disk
        fs_inode new_inode;
        strcpy(new_inode.owner, username.c_str());
        new_inode.size = 0;
        new_inode.type = etc[0];
        int wrt = get_write();
        if (wrt < 0)
        {
            return -1;
        }
        write(wrt, (void *)&new_inode);
        // 2) add fs_direntry to directory
        fs_direntry fileblock[FS_DIRENTRIES];
        int block = -1;
        int pos = -1;
        for (uint32_t i = 0; i < directory.size; i++)
        {
            read(directory.blocks[i], (void *)&fileblock);
            for (uint32_t j = 0; j < FS_DIRENTRIES; j++)
            {
                if (strcmp(fileblock[j].name, filename.c_str()) == 0)
                {
                    return -1;
                }
                if (fileblock[j].inode_block == 0)
                {
                    block = i;
                    pos = j;
                    i = directory.size;
                    j = FS_DIRENTRIES;
                }
            }
        }
        if (block < 0)
        {
            if (directory.size == FS_MAXFILEBLOCKS)
            {
                return -1;
            }
            fileblock[0].inode_block = wrt;
            strcpy(fileblock[0].name, filename.c_str());
            fileblock[1].inode_block = 0;
            int wrt = get_write();
            if (wrt < 0)
            {
                return -1;
            }
            write(wrt, (void *)&fileblock);
            directory.blocks[directory.size] = wrt;
            directory.size = directory.size + 1;
            write(at, (void *)&directory);
        }
        else
        {
            fileblock[pos].inode_block = wrt;
            strcpy(fileblock[pos].name, filename.c_str());
            pos++;
            if (pos < static_cast<int>(FS_DIRENTRIES))
            {
                fileblock[pos].inode_block = 0;
            }
            write(directory.blocks[block], (void *)&fileblock);
        }
        return 0;
    }

    int del()
    {
        int at = find_dirblock(false);
        if (at < 0)
        {
            return -1;
        }

        // 0) get filename
        size_t i = pathname.size();
        i--;
        while (pathname[i] != '/')
        {
            i--;
        }
        i++;
        const char *filename = pathname.substr(i, pathname.size() - i).c_str();
        // 1) search root directory for file
        int inode, block, pos = -1;
        fs_inode root;
        read(at, (void *)&root);

        fs_direntry dirs[FS_DIRENTRIES];
        for (uint32_t i = 0; i < root.size; i++)
        {
            read(root.blocks[i], (void *)&dirs);
            // 3) search each direntry
            for (uint32_t j = 0; j < FS_DIRENTRIES; j++)
            {
                // looks for dir
                if (dirs[j].inode_block == 0)
                {
                    return -1;
                }
                if (strcmp(dirs[j].name, filename) == 0)
                {
                    inode = dirs[j].inode_block;
                    block = i;
                    pos = j;
                    i = root.size;
                    j = FS_DIRENTRIES;
                }
            }
        }
        // 3) check directory to be deleted
        fs_inode leaf;
        read(inode, (void *)&leaf);
        if (strcmp(leaf.owner, username.c_str()))
        {
            return -1;
        }
        if (leaf.type == 'd' && leaf.size > 0)
        {
            return -1;
        }
        // 4) delete directory & replace with last value
        if (block < static_cast<int>(root.size - 1))
        {
            // load last block
            fs_direntry last[FS_DIRENTRIES];
            read(root.blocks[root.size - 1], (void *)&last);
            // find last value
            uint32_t end = 0;
            for (uint32_t i = 0; i < FS_DIRENTRIES; i++)
            {
                if (last[i].inode_block == 0)
                {
                    end = i - 1;
                    i = FS_DIRENTRIES;
                }
            }
            strcpy(dirs[pos].name, last[end].name);
            dirs[pos].inode_block = last[end].inode_block;
            last[end].inode_block = 0;
            // remove inode
            empty_blocks.insert(inode);
            // rewrite current block
            write(root.blocks[block], (void *)&dirs);
            // rewrite last block
            if (end == 0)
            {
                root.size -= 1;
                empty_blocks.insert(root.blocks[root.size]);
                write(at, (void *)&root);
                return 0;
            }
            write(root.blocks[root.size - 1], (void *)&last);
        }
        else
        {
            uint32_t end = 0;
            for (uint32_t i = 0; i < FS_DIRENTRIES; i++)
            {
                if (dirs[i].inode_block == 0)
                {
                    end = i - 1;
                    i = FS_DIRENTRIES;
                }
            }
            // remove inode
            empty_blocks.insert(inode);
            if (end == 0)
            {
                root.size -= 1;
                empty_blocks.insert(root.blocks[root.size]);
                write(at, (void *)&root);
                return 0;
            }
            strcpy(dirs[pos].name, dirs[end].name);
            dirs[pos].inode_block = dirs[end].inode_block;
            dirs[end].inode_block = 0;
            // rewrite current block
            write(root.blocks[block], (void *)&dirs);
        }
        return 0;
    }

    void get_message(std::string &mssg, bool e, bool d)
    {
        mssg = command + " " + username + " " + pathname;
        if (e)
        {
            mssg = mssg + " " + etc;
        }
        if (d)
        {
            mssg = mssg + '\0' + data;
        }
        mssg = mssg + '\0';
    }
};

void handle(int sock)
{
    Request request;

    int status = -1;
    std::string mssg = "";
    switch (request.receive(sock))
    {
    case DO::READFILE:
        status = request.readfile();
        request.get_message(mssg, true, true);
        break;
    case DO::WRITEFILE:
        status = request.writefile();
        request.get_message(mssg, true, false);
        break;
    case DO::CREATEFILE:
        status = request.create();
        request.get_message(mssg, true, false);
        break;
    case DO::DELETEFILE:
        status = request.del();
        request.get_message(mssg, false, false);
        break;
    }

    if (status == 0)
    { // if (no errors), then send reply
        send(sock, mssg.c_str(), mssg.size(), 0);
    }
    close(sock);
}

int init_files()
{
    // init empty_blocks
    for (uint32_t i = 0; i < FS_DISKSIZE; i++)
    {
        empty_blocks.insert(i);
    }

    // remove blocks from empty_blocks
    fs_inode root;
    fs_direntry block[FS_DIRENTRIES];
    std::vector<uint32_t> list;
    list.push_back(0);
    size_t i = 0;
    while (i < list.size())
    {
        disk_readblock(list[i], (void *)&root);
        if (root.type == 'f')
        {
            for (uint32_t j = 0; j < root.size; j++)
            {
                // remove file data block
                empty_blocks.erase(root.blocks[j]);
            }
        }
        if (root.type == 'd')
        {
            for (uint32_t j = 0; j < root.size; j++)
            {
                // remove direntry block
                empty_blocks.erase(root.blocks[j]);
                // read block
                disk_readblock(root.blocks[j], (void *)&block);
                for (uint32_t k = 0; k < FS_DIRENTRIES; k++)
                {
                    if (block[k].inode_block == 0)
                    {
                        k = FS_DIRENTRIES;
                    }
                    else
                    {
                        list.push_back(block[k].inode_block);
                    }
                }
            }
        }
        i++;
    }
    auto ptr = list.begin();
    while (ptr != list.end())
    {
        empty_blocks.erase(*ptr);
        ptr++;
    }
    return 0;
}

int make_sock(int port_number)
{
    // make socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        perror("Error opening socket");
        exit(1);
    }
    // set socket options
    int t = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (void *)&t, sizeof(t)) < 0)
    {
        perror("Error setting socket option");
        close(sock);
        exit(1);
    }
    // bind socket
    sockaddr_in addr{}; // initializes with zeroes
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port_number);
    if (bind(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
    {
        perror("Error binding socket");
        close(sock);
        exit(1);
    }
    if (listen(sock, 30) < 0)
    {
        perror("Error listening on socket");
        close(sock);
        exit(1);
    }
    // bind socket
    sockaddr_in localAddr;
    socklen_t addrLen = sizeof(localAddr);
    getsockname(sock, reinterpret_cast<sockaddr *>(&localAddr), &addrLen);
    port_number = ntohs(localAddr.sin_port);
    print_port(port_number);
    return sock;
}

int search_direntry(uint32_t at, const char *dir)
{
    // loads disk_readblock(at) as directory
    fs_direntry dirs[FS_DIRENTRIES];
    disk_readblock(at, (void *)&dirs);
    for (uint32_t i = 0; i < FS_DIRENTRIES; i++)
    {
        // looks for dir in owners
        if (strcmp(dirs[i].name, dir) == 0)
        {
            return dirs[i].inode_block;
        }
    }
    return -1;
}

int main(int argc, char *argv[])
{
    // initialize
    init_files();
    //  port
    int port_number = 0;
    if (argc == 3)
    {
        port_number = atoi(argv[2]);
    }
    // make socket
    int sock = make_sock(port_number);

    while (true)
    {
        // Accept connection
        sockaddr_in client_address{};
        int new_sock;
        socklen_t client_len = sizeof(client_address);
        new_sock = accept(sock, (struct sockaddr *)&client_address, &client_len);
        fcntl(new_sock, F_SETFL, O_NONBLOCK);
        // Receive data
        // TODO: make thread with function handler & argument new_sock
        // boost::thread(handler, new_sock);Request req;
        boost::thread(handle, new_sock).detach();
        // req.handle(new_sock);
        // TODO: yield to thread
    }
    return 0;
}