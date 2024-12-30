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
std::set<uint32_t> blockset;
boost::shared_mutex mutex;

uint32_t wrt = 1;
void get_write()
{
    while (blockset.find(wrt) != blockset.end())
    {
        if (wrt == FS_DISKSIZE)
        {
            wrt = 1;
        }
        wrt++;
    }
}

void write_helper(const char *out)
{
    cout_lock.lock();
    std::cout << out << std::endl;
    cout_lock.unlock();
}

class Request
{
private:
    std::string command = "";
    std::string username = "";
    std::string pathname = "";
    std::string etc = "";
    std::string data = "";
    std::string mssg = "";

    void read(int at, void *ptr)
    {
        std::stringstream ss;
        ss << "Reading at block " << at;
        write_helper(ss.str().c_str());
        boost::shared_lock<boost::shared_mutex> lock(mutex);
        disk_readblock(at, ptr);
    }
    void write(int at, void *ptr)
    {
        boost::unique_lock<boost::shared_mutex> unique_lock(mutex);
        disk_writeblock(at, ptr);
    }

    void receive(int sock)
    {
        // receive message
        int bytes_received = 0;
        char buffer[BUFFER_SIZE];
        bytes_received = recv(sock, buffer, BUFFER_SIZE - 1, 0);
        // command
        int i = 0;
        while (buffer[i] != ' ')
        {
            command += buffer[i];
            mssg += buffer[i];
            i++;
        }
        mssg += buffer[i];
        i++;
        // username
        while (buffer[i] != ' ')
        {
            username += buffer[i];
            mssg += buffer[i];
            i++;
        }
        mssg += buffer[i];
        i++;
        // directory
        //      delete
        if (command == FS_DELETE)
        {
            while (buffer[i] != '\0')
            {
                Request::pathname += buffer[i];
                Request::mssg += buffer[i];
                i++;
            }
            Request::mssg += buffer[i];
            return;
        }
        //      readblock, writeblock, create
        while (buffer[i] != ' ')
        {
            Request::pathname += buffer[i];
            Request::mssg += buffer[i];
            i++;
        }
        Request::mssg += buffer[i];
        i++;
        // etc...
        while (buffer[i] != '\0')
        {
            Request::etc += buffer[i];
            Request::mssg += buffer[i];
            i++;
        }
        Request::mssg += buffer[i];
        // writeblock data
        if (command == FS_WRITEBLOCK)
        {
            i++;
            while (i < bytes_received)
            {
                data += buffer[i];
                i++;
            }
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
        while (i < root.size)
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
        char buffer[FS_BLOCKSIZE + 1];
        read(file.blocks[block], (void *)&buffer);
        // disk_readblock(file.blocks[block], (void *)&buffer);
        buffer[FS_BLOCKSIZE] = '\0';
        data = buffer;
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
        // disk_readblock(at, (void *)&file);
        if (file.type != 'f')
        {
            return -1;
        }
        if (strcmp(file.owner, username.c_str()))
        {
            return -1;
        }

        if (block < file.size)
        {
            write(file.blocks[block], (void *)data.c_str());
            // disk_writeblock(file.blocks[block], (void *)data.c_str());
            return 0;
        }
        if (block == file.size)
        {
            get_write();
            write(wrt, (void *)data.c_str());
            // disk_writeblock(wrt, (void *)data.c_str());
            blockset.insert(wrt);
            return wrt;
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
        // disk_readblock(at, (void *)&directory);
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
        get_write();
        write(wrt, (void *)&new_inode);
        // disk_writeblock(wrt, (void *)&new_inode);
        // 2) add fs_direntry to directory
        fs_direntry new_direntry;
        strcpy(new_direntry.name, filename.c_str());
        new_direntry.inode_block = wrt;
        fs_direntry fileblock[FS_DIRENTRIES];
        int block, pos = -1;
        for (uint32_t i = 0; i < directory.size; i++)
        {
            read(directory.blocks[i], (void *)&fileblock);
            // disk_readblock(directory.blocks[i], (void *)&fileblock);
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
                }
            }
        }
        if (block < 0)
        {
            fileblock[0].inode_block = wrt;
            strcpy(fileblock[0].name, filename.c_str());
            fileblock[1].inode_block = 0;
            get_write();
            write(wrt, (void *)&fileblock);
            // disk_writeblock(wrt, (void *)&fileblock);
            directory.blocks[directory.size] = wrt;
            directory.size += 1;
            write(at, (void *)&directory);
            // disk_writeblock(at, (void *)&directory);
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
            write(directory.blocks[block], fileblock);
            // disk_writeblock(directory.blocks[block], fileblock);
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
        // disk_readblock(at, (void *)&root);

        fs_direntry dirs[FS_DIRENTRIES];
        for (uint32_t i = 0; i < root.size; i++)
        {
            read(at, (void *)&root);
            // disk_readblock(root.blocks[i], (void *)&dirs);
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
                }
            }
        }
        // 3) check directory to be deleted
        fs_inode leaf;
        read(inode, (void *)&leaf);
        // disk_readblock(inode, (void *)&leaf);
        if (strcmp(leaf.owner, username.c_str()))
        {
            return -1;
        }
        if (leaf.size > 0)
        {
            return -1;
        }
        // 4) delete directory & replace with last value
        if (block < static_cast<int>(root.size - 1))
        {
            // load last block
            fs_direntry last[FS_DIRENTRIES];
            read(root.blocks[root.size - 1], (void *)&last);
            // disk_readblock(root.blocks[root.size - 1], (void *)&last);
            // find last value
            uint32_t end = 0;
            for (uint32_t i = 0; i < FS_DIRENTRIES; i++)
            {
                if (last[i].inode_block == 0)
                {
                    end = i - 1;
                }
            }
            strcpy(dirs[pos].name, last[end].name);
            dirs[pos].inode_block = last[end].inode_block;
            last[end].inode_block = 0;
            // remove inode
            blockset.erase(inode);
            // rewrite current block
            write(root.blocks[block], (void *)&dirs);
            // disk_writeblock(root.blocks[block], (void *)&dirs);
            // rewrite last block
            if (end == 0)
            {
                root.size -= 1;
                write(at, (void *)&root);
                // disk_writeblock(at, (void *)&root);
                return 0;
            }
            write(root.blocks[root.size - 1], (void *)&last);
            // disk_writeblock(root.blocks[root.size - 1], (void *)&last);
        }
        else
        {
            uint32_t end = 0;
            for (uint32_t i = 0; i < FS_DIRENTRIES; i++)
            {
                if (dirs[i].inode_block == 0)
                {
                    end = i - 1;
                }
            }
            // remove inode
            blockset.erase(inode);
            if (end == 0)
            {
                root.size -= 1;
                write(at, (void *)&root);
                // disk_writeblock(at, (void *)&root);
                return 0;
            }
            strcpy(dirs[pos].name, dirs[end].name);
            dirs[pos].inode_block = dirs[end].inode_block;
            dirs[end].inode_block = 0;
            // rewrite current block
            write(root.blocks[block], (void *)&dirs);
            // disk_writeblock(root.blocks[block], (void *)&dirs);
        }
        return 0;
    }

public:
    void run(int sock)
    { // called after accepting connection
        // 1) get whole message & parse command
        receive(sock);
        mssg = command + " " + username + " " + pathname;
        // 2) check values
        if (username.size() > FS_MAXUSERNAME)
        {
            close(sock);
            return;
        }
        // 3) call handlers
        int status = -1;
        if (command == FS_READBLOCK)
        {
            status = readfile();
            mssg = mssg + " " + etc + '\0' + data;
        }
        else if (command == FS_WRITEBLOCK)
        {
            status = writefile();
            mssg = mssg + " " + etc + '\0';
        }
        else if (command == FS_CREATE)
        {
            status = create();
            mssg = mssg + " " + etc + '\0';
        }
        else if (command == FS_DELETE)
        {
            status = del();
            mssg += '\0';
        }
        if (status)
        {
            close(sock);
            return;
        }
        // 3) send return message
        printf("Returning with: %s\n", mssg.c_str());
        send(sock, mssg.c_str(), mssg.size(), 0);
        close(sock);
    }
};

void handle(int sock)
{
    Request new_request;
    new_request.run(sock);
}
/*
int read_check(int at, fs_inode &root, const char *owner, uint32_t b)
{
    // root is not file
    if (root.type != 'f')
    {
        return -1;
    }
    // owner != owner of file
    if (at > 0 && strcmp(root.owner, owner) != 0)
    {
        return -1;
    }
    // block > number of blocks in file
    if (b >= root.size)
    {
        return -1;
    }
    return 0;
}

int read(int at, uint32_t b, const char *owner, char data[])
{
    fs_inode file;
    // TODO: get file mutex
    disk_readblock(at, (void *)&file);
    //      file error checking
    if (read_check(at, file, owner, b) < 0)
    {
        return -1;
    }
    // readblock -- socket, block to read from
    disk_readblock(at, (void *)&data);
    // TODO: release file mutex
    return 0;
}

int write_check(fs_inode &root, const char *owner, uint32_t b)
{
    // dir does not lead to file
    if (root.type != 'f')
    {
        return -1;
    }
    // owner != owner of file
    if (strcmp(root.owner, owner) != 0)
    {
        return -1;
    }
    // block > number of blocks in file + 1
    if (b > root.size)
    {
        return -1;
    }
    return 0;
}

int create_check(int at, fs_inode &root, const char *owner, char t)
{
    printf("In create_check!\n");
    // owner != owner of file
    if (at > 0 && strcmp(root.owner, owner) != 0)
    {
        printf("Error at owner, root (%s) & owner (%s)\n", root.owner, owner);
        return -1;
    }
    // file is not directory
    if (root.type != 'd')
    {
        printf("Error at root != directory\n");
        return -1;
    }
    if (t != 'f' && t != 'd')
    {
        printf("Error at created type\n");
        return -1;
    }
    return 0;
}

int create(fs_inode &root, const char *owner, const char *name, char t)
{
    // figure out where to add fs_direntry for new file/directory
    fs_direntry block[FS_DIRENTRIES];
    int at = -1;
    int pos = root.size - 1;
    printf("Create initialized with at %i and pos %i\n", at, pos);
    bool addblock = false;
    if (root.size > 0)
    {
        printf("Looking into root entries!\n");
        disk_readblock(root.blocks[pos], (void *)&block);
        for (uint32_t i = 0; i < FS_DIRENTRIES; i++)
        {
            if (block[i].inode_block == 0)
            {
                at = i;
                i = FS_DIRENTRIES;
            }
        }
        if (at < 0)
        {
            addblock = true;
            pos++;
        }
        if (pos == FS_DIRENTRIES && addblock)
        {
            return -1;
        }
    }
    else
    {
        printf("Empty directory!");
        pos = 0;
        at = 0;
        addblock = true;
    }
    // write inode to disk
    fs_inode file;
    file.type = t;
    strcpy(file.owner, owner);
    file.size = 0;
    //      get next writing block
    get_write();
    //      write to disk
    disk_writeblock(wrt, (void *)&file);
    blockset.insert(wrt);

    // write block to disk
    block[at].inode_block = wrt;
    strcpy(block[at].name, name);
    //      fix next direntry
    if (static_cast<unsigned int>(at) < FS_DIRENTRIES - 1)
    {
        block[at + 1].inode_block = 0;
    }
    //      fix block added
    if (addblock)
    {
        wrt++;
        get_write();
        root.blocks[pos] = wrt;
        root.size += 1;
    }
    printf("Rewriting block direntry at %u, with name %s and inode %u\n", root.blocks[pos], block[at].name, block[at].inode_block);
    disk_writeblock(root.blocks[pos], (void *)&block);
    blockset.insert(wrt);
    wrt++;

    if (addblock)
    {
        printf("Returning addblock true!\n");
        return 1;
    }
    printf("Returning addblock false!\n");
    return 0;
}

int del_check(fs_inode &root, const char *owner)
{
    // file is not directory
    if (root.type != 'd')
    {
        return -1;
    }
    // owner != owner of root
    if (strcmp(root.owner, owner) != 0)
    {
        return -1;
    }
    // does not contain any files
    if (root.size == 0)
    {
        return -1;
    }
    return 0;
}

int del(fs_inode root, const char *child)
{
    if (root.type == 'd' && root.size != 0)
    {
        return -1;
    }
    // remove child directory from blockset
    int at, pos = -1;
    fs_direntry block[FS_DIRENTRIES];
    for (uint32_t i = 0; i < root.size; i++)
    {
        disk_readblock(root.blocks[i], block);
        for (uint32_t j = 0; j < FS_DIRENTRIES; j++)
        {
            // looks for dir in owners
            if (strcmp(block[j].name, child) == 0)
            {
                pos = j;
                at = i;
                j = FS_DIRENTRIES;
                i = FS_MAXFILEBLOCKS;
            }
        }
    }
    if (pos < 0)
    {
        return -1;
    }
    // TODO: if directory, make sure it is empty
    // TODO: if file, delete all children from blockset
    blockset.erase(block[pos].inode_block);

    // fix up directories
    bool del_entry = false;
    //      move last entry to replace deleted entry
    if (at == static_cast<int>(root.size) - 1)
    {
        // shuffle within array
        if (pos == 0)
        {
            del_entry = true;
        }
        else
        {
            int lst = -1;
            for (uint32_t i = 0; i < FS_DIRENTRIES; i++)
            {
                if (block[i].inode_block == 0)
                {
                    lst = i - 1;
                    i = FS_DIRENTRIES;
                }
            }
            if (lst == -1)
            {
                lst = FS_DIRENTRIES - 1;
            }
            block[pos].inode_block = block[lst].inode_block;
            strcpy(block[pos].name, block[lst].name);
            block[lst].inode_block = 0;
            disk_writeblock(root.blocks[at], (void *)&block);
        }
    }
    else
    {
        // find last entry
        fs_direntry last[FS_DIRENTRIES];
        disk_readblock(root.blocks[root.size - 1], (void *)&last);
        int lst = -1;
        for (uint32_t i = 0; i < FS_DIRENTRIES; i++)
        {
            if (last[i].inode_block == 0)
            {
                lst = i - 1;
                i = FS_DIRENTRIES;
            }
        }
        if (lst < 0)
        {
            lst = FS_DIRENTRIES - 1;
        }
        if (lst == 0)
        {
            del_entry = true;
        }
        // set deleted entry to last entry & write to disk
        block[pos].inode_block = last[lst].inode_block;
        strcpy(block[pos].name, last[lst].name);
        disk_writeblock(root.blocks[at], (void *)&block);
        // set last entry to zero & write to disk
        if (!del_entry)
        {
            last[lst].inode_block = 0;
            disk_writeblock(root.blocks[root.size - 1], (void *)&last);
        }
    }
    if (del_entry)
    {
        blockset.erase(root.blocks[root.size - 1]);
        root.size -= 1;
        return 1;
    }
    return 0;
}
*/

int init_files()
{
    fs_inode root;
    fs_direntry block[FS_DIRENTRIES];
    // add blocks to blockset
    std::vector<int> list;
    list.push_back(0);
    size_t i = 0;
    while (i < list.size())
    {
        disk_readblock(list[i], (void *)&root);
        if (root.type == 'f')
        {
            for (uint32_t j = 0; j < root.size; j++)
            {
                blockset.insert(root.blocks[j]);
            }
        }
        if (root.type == 'd')
        {
            for (uint32_t j = 0; j < root.size; j++)
            {
                // add root.blocks[j] to blockset
                blockset.insert(root.blocks[j]);
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
    blockset.insert(list.begin(), list.end());
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

/*
int find_dirblock(std::string &dir)
{
    int i = 1;
    int at = -1;
    std::string cur = "";
    fs_inode root;
    if (dir.size() == 0)
    {
        return 0;
    }
    printf("Find_dirblock read 1, reading block: 0\n");
    disk_readblock(0, (void *)&root);
    // at = directory home to end of dir
    // cur = name of final directory/file
    int size = dir.size();
    while (i < size)
    {
        if (dir[i] == '/')
        {
            if (root.type == 'f')
            {
                return -1;
            }
            for (uint32_t j = 0; j < root.size; j++)
            {
                at = search_direntry(root.blocks[j], cur.c_str());
                if (at >= 0)
                {
                    j = FS_MAXFILEBLOCKS;
                }
            }
            if (at < 0)
            {
                return -1;
            }
            printf("Find_dirblock read 2, reading block: %i\n", at);
            disk_readblock(at, (void *)&root);
            cur = "";
        }
        else
        {
            cur += dir[i];
        }
        i++;
    }
    // at = directory/file at end of dir
    if (at > 0)
    {
        printf("Find_dirblock read 3, reading block: %i\n", at);
        disk_readblock(at, (void *)&root);
    }
    for (uint32_t j = 0; j < root.size; j++)
    {
        at = search_direntry(root.blocks[j], cur.c_str());
        if (at >= 0)
        {
            j = FS_MAXFILEBLOCKS;
        }
    }
    return at;
}

int read_helper(int bytes, int b, int d, char data[], char buffer[])
{
    printf("Calling read_helper at buffer byte %i and data byte %d\n", b, d);
    while (b < bytes)
    {
        data[d] = buffer[b];
        if (data[d] == '\0')
        {
            return d;
        }
        b++;
        d++;
        if (static_cast<unsigned int>(d) == FS_BLOCKSIZE - b)
        {
            b = bytes;
        }
    }
    return d;
}
*/

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