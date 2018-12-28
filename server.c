#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <malloc.h>
#include <time.h>

#define QUEUE_SIZE 100
#define CMD_LENGTH 12
#define ARGUMENT_LENGTH 1024
#define PATH_LENGTH 512
#define EXTENSION_LENGTH 5
#define DEFAULT_SIZE 1000
#define RESPONSE_SIZE 100000
#define DOCUMENT_ROOT "/var/www"                //网站根目录
#define LOG_PATH "/var/www/log.txt"             //日志文件目录

typedef struct request{                         //请求结构体
    char cmd[CMD_LENGTH];                       //请求方式，GET、POST等
    char path[PATH_LENGTH];                     //uri路径信息
    char arg[ARGUMENT_LENGTH];                  //请求附带参数
    char extension[EXTENSION_LENGTH];           //文件名后缀
}rq;

typedef struct data_stream{                     //二进制数据结构体
    int length;                                 //已使用的内存长度
    char* data;                                 //数据内存空间指针
}ds;

int make_server_socket(int);
void child_waiter(int);
void parse_request(const char*, rq*);
void process_rq(int, const char*);
int process_php(const char*, const char*, ds*);
int process_static(const char*, ds*);
void process_status(int, ds*);
void concat(ds*, ds*);
void log(struct sockaddr_in*, const char*);

int main(void)
{
    int server_socket;                                  
    socklen_t addrlen;                                  
    struct sockaddr_in client_addr;
    char buffer[DEFAULT_SIZE];

    //打开80端口进行监听
    server_socket = make_server_socket(80);
    if(server_socket < 0){
        perror("MAKE SOCKET ERROR");
        exit(EXIT_FAILURE);
    }

    addrlen = sizeof(client_addr);

    //将子进程处理函数与信号进行绑定
    signal(SIGCHLD, child_waiter);

    while(1){
        int client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &addrlen);

        //当有新的客户端访问服务器，创建新进程进行处理
        int pid = fork();

        switch(pid){
            case 0:
                {
                    //子进程中关闭多余的socket接口
                    close(server_socket);

                    //如果accept失败，终止该子进程
                    if(client_socket < 0){
                        perror("Server Accept Failed");
                        exit(EXIT_FAILURE);
                    }

                    //读取请求信息
                    FILE *fp = fdopen(client_socket, "r");
                    fgets(buffer, DEFAULT_SIZE, fp);

                    //记录此次访问
                    log(&client_addr, buffer);

                    //处理此次客户端访问请求
                    process_rq(client_socket, buffer);

                    break;
                }
            case -1:
                {
                    //创建进程失败，结束程序
                    perror("FORK FAILED");

                    exit(EXIT_FAILURE);

                    break;
                }
            default:
                {
                    //父进程关闭多余的socket接口
                    close(client_socket);
                }
        }
    }

    //关闭服务器监听socket接口
    close(server_socket);

    return 0;
}

/*******************处理请求****************/
void process_rq(int socket, const char* request_str)
{
    int status = 500;                       //定义状态返回码
    rq request;                             //定义请求结构体并初始化
    ds content, response;                   //定义数据流载荷结构体
    bzero(&request, sizeof(request));       //初始化
    bzero(&content, sizeof(content));       //初始化
    bzero(&response, sizeof(response));     //初始化

    //解析HTTP请求
    parse_request(request_str, &request); 

    //加入根目录，形成绝对路径
    char tmp[strlen(request.path)];
    strcpy(tmp, request.path);
    sprintf(request.path, "%s%s", DOCUMENT_ROOT, tmp);

    //为响应载荷分配内存
    response.data = (char*)malloc(RESPONSE_SIZE);
    bzero(response.data, RESPONSE_SIZE);

    //定义响应HTTP头格式
    const char* http_header_format = (const char*)"HTTP/1.0 %3d OK\r\nServer: CoolKid Web Server\r\n";

    //如果HTTP请求方法为GET方法
    if(strcmp(request.cmd, "GET") == 0){

        if(strncmp(request.extension, "php", 3) == 0){

            //若请求为php页面，进行php解析
            status = process_php(request.path, request.arg, &content);

            //根据返回状态码进一步处理
            process_status(status, &content);

        }else{

            //若请求为静态文件，进行文件传输
            status = process_static(request.path, &content); 

            //根据返回状态码进一步处理
            process_status(status, &content);

            //为content前面添加一个换行符
            char* tmp = (char*)malloc(content.length + strlen("\r\n"));
            strcpy(tmp, "\r\n");
            memcpy(tmp + strlen("\r\n"), content.data, content.length);
            free(content.data);
            content.data = tmp;
            content.length += strlen("\r\n");
        }
    }

    //将http头部写入响应载荷
    sprintf(response.data, http_header_format, status);
    response.length = strlen(response.data);

    //将http头部与content的内容连接起来
    concat(&response, &content);

    //将响应内容写回socket
    write(socket, response.data, response.length);

    //释放申请的内存
    free(content.data);
    free(response.data);

    //退出子进程
    exit(0);
}

/***************处理静态请求******************/
int process_static(const char* path, ds* content)
{
    ds buffer;                                  //定义缓冲变量
    struct stat buf;                            //文件信息变量

    //如果获取文件信息失败，返回500错误
    if(stat(path, &buf) == -1){
        return 404;
    }

    //如果目标路径是一个目录
    if(S_ISDIR(buf.st_mode)){

        DIR* dp;                                
        struct dirent *entry;                   

        //为载荷申请内存空间
        content->data = (char*)malloc(DEFAULT_SIZE);

        //载荷填充html页面的部分信息
        sprintf(content->data, "<html>\n\t<head>\n\t\t<title>Index of %s</title>\n\t</head>\n\
                \t<body>\n\t\t<h1>Index of %s</h1>\n\t\t<table>\n", &path[8], &path[8]);
        content->length = strlen(content->data);

        //若打开目录失败，返回500错误
        if((dp = opendir(path)) == NULL){
            free(content->data);
            return 500;
        }

        //进入目标目录
        chdir(path);

        //为缓冲区申请内存空间
        buffer.data = (char*)malloc(DEFAULT_SIZE);

        //循环读取目录内容条目
        while((entry = readdir(dp)) != NULL){

            //初始化缓冲区
            bzero(buffer.data, DEFAULT_SIZE);

            //获取当前条目的信息
            lstat(entry->d_name, &buf);

            if(S_ISDIR(buf.st_mode)){

                //如果当前条目为文件夹
                sprintf(buffer.data, "\t\t\t<tr><td><a href=\"%s/\"/>%s/</a></td></tr>\n", entry->d_name, entry->d_name); 

            }else{

                //如果当前条目为普通文件
                sprintf(buffer.data, "\t\t\t<tr><td><a href=\"%s\">%s</a></td></tr>\n", entry->d_name, entry->d_name);

            }

            buffer.length = strlen(buffer.data);

            //将格式化后的目录条目信息填充到载荷
            concat(content, &buffer);
        }

        //初始化缓冲区
        bzero(buffer.data, DEFAULT_SIZE);

        //将html页面结尾信息填入缓冲区
        strcpy(buffer.data, "\t\t</table>\t\n</body>\n</html>");
        buffer.length = strlen(buffer.data);

        //将缓冲区填入载荷
        concat(content, &buffer);

        //返回上级目录
        chdir("..");

        //关闭文件夹流
        closedir(dp);

    }else if(S_ISREG(buf.st_mode)){

        //如果目标路径是一个文件
        //为载荷申请跟文件大小一样的内存空间
        content->data = (char*)malloc(buf.st_size);

        //打开文件流
        FILE* fp = fopen(path, "rb");

        //打开失败，返回服务器500错误
        if(fp == NULL){
            free(content->data);
            return 500;
        }

        //将文件内容读入载荷
        fread(content->data, sizeof(char), buf.st_size, fp);
        content->length = buf.st_size;

        //关闭文件流
        fclose(fp);
    }
    return 200;
}

/****************处理php请求***************/
int process_php(const char* path, const char* arg, ds* content)
{
    ds buffer;                                  //定义缓冲区
    bzero(&buffer, sizeof(buffer));             //初始化缓冲区

    //定义bash调用格式
    char *format = (char*)"QUERY_STRING=\"%s\" REDIRECT_STATUS=200 SCRIPT_FILENAME=\"%s\" php-cgi";

    //定义指令变量
    char command[strlen(format) + strlen(path) + strlen(arg)];

    //生成调用php-cgi解析php的指令
    sprintf(command, format, arg, path);

    //为缓冲区申请内存空间
    buffer.data = (char*)malloc(DEFAULT_SIZE);

    //使用popen方式创建进程调用bash执行php-cgi
    //并以文件流方式读取解析结果
    FILE* fp = popen(command, "r");

    //如果失败，返回服务器500错误
    if(fp == NULL){
        return 500;
    } 

    //循环读取解析结果
    while(!feof(fp)){

        //初始化缓冲区
        bzero(buffer.data, DEFAULT_SIZE);

        //将一行解析结果读入缓冲区
        fgets(buffer.data, DEFAULT_SIZE, fp);
        buffer.length = strlen(buffer.data);

        //将缓冲区内容填充进载荷
        concat(content, &buffer);
    }

    //释放缓冲区的内存空间
    free(buffer.data);

    //关闭文件流
    pclose(fp);

    return 200;
}

/**************子进程处理，避免僵尸进程********/
void child_waiter(int signum)
{
    pid_t pid;
    int stat;

    //使用waitpid方式，避免阻塞
    while((pid = waitpid(-1, &stat, WNOHANG)) > 0)
        printf("child %d terminated.\n", pid);
}

/****************解析请求method和uri************/
void parse_request(const char* str, rq* request)
{
    //初始化参数中的请求结构体
    bzero(request, sizeof(rq));

    //定义缓冲区
    char buffer[PATH_LENGTH + ARGUMENT_LENGTH];

    //将参数中的请求方法读取出来
    //其余内容读入缓冲区
    sscanf(str, "%s%s", request->cmd, buffer);

    //定义两个位置指针
    const char *pos1, *pos2;

    //将这两个指针指向缓冲区
    pos1 = pos2 = buffer;

    //以'/'字符作为标识，循环将'/'左侧的内容读入路径
    while(pos1 = strchr(pos1, '/')){
        strncpy(request->path + (pos2 - buffer), pos2, pos1 - pos2);
        pos2 = pos1++; 
    }

    //判断是否存在'.'，作为标识分割后缀名
    if(pos1 = strchr(pos2, '.')){

        //若存在'.'，则将'.'之前的内容作为路径信息读入
        strncpy(request->path + (pos2 - buffer), pos2, pos1 -  pos2);
        pos2 = pos1++;

        if(pos1 = strchr(pos2, '?')){

            //若存在'?'，则'?'之前的内容作为路径信息，后面的作为参数信息保存
            strncpy(request->path + (pos2 - buffer), pos2, (pos1 - pos2) > PATH_LENGTH ? PATH_LENGTH : (pos1 - pos2));
            strncpy(request->extension, ++pos2, (pos1 - pos2) - 1 > EXTENSION_LENGTH ? EXTENSION_LENGTH : (pos1 - pos2) - 1);
            strncpy(request->arg, ++pos1, ARGUMENT_LENGTH);

        }else{

            //若不存在'?'，则'.'之后的全部内容作为路径信息读入
            strncpy(request->path + (pos2 - buffer), pos2, PATH_LENGTH - strlen(request->path));
            strncpy(request->extension, ++pos2, EXTENSION_LENGTH);

        }
    }else{

        //若不存在'.'，则将缓冲区剩下的内容全部读入路径信息
        strncpy(request->path + (pos2 - buffer), pos2, PATH_LENGTH - strlen(request->path));
    }
}

/****************打开指定端口的socket接口**************/
int make_server_socket(int portnum)
{
    int s;                                                          //socket描述符
    struct sockaddr_in server;                                      //服务器地址

    //打开socket失败，返回-1
    if((s = socket(AF_INET, SOCK_STREAM, 0)) < 0) return -1;

    //初始化服务器地址
    memset((char*)&server, 0, sizeof(server));

    //设置服务器地址
    server.sin_family = AF_INET;
    server.sin_port = htons(portnum);
    server.sin_addr.s_addr = htonl(INADDR_ANY);

    //将socket与服务器地址进行绑定
    if(bind(s, (struct sockaddr*)&server, sizeof(server)) < 0) return -1;

    //监听该socket，队列大小为QUEUE_SIZE
    if(listen(s, QUEUE_SIZE)) return -1;

    return s;
}

/***************处理错误页面*******************************/
void process_status(int status, ds* content)
{
    char path[20];

    switch(status){
        case 200:
            return;
        case 404:
            bzero(content, sizeof(*content));
            strcpy(path, (const char*)"/var/www/404.html"); 
            break;
        default:
            bzero(content, sizeof(*content));
            strcpy(path, (const char*)"/var/www/500.html");
            break;
    }

    process_static(path, content);
}

/***************连接两个二进制数据流结构体*******************/
void concat(ds* stream1, ds* stream2)
{
    //获取数据流1的内存大小空间
    int size = malloc_usable_size(stream1->data);

    //如果数据流1的剩余空间不够用，重新申请内存
    if(stream1->length + stream2->length >= size){
        stream1->data = (char*)realloc(stream1->data, stream1->length + stream2->length); 
    }    

    //以memcpy的方式进行二进制数据流的拼接
    memcpy(stream1->data + stream1->length, stream2->data, stream2->length);
    stream1->length += stream2->length;
}

/*****************将请求记录在日志文件中**********************/
void log(struct sockaddr_in* client_addr, const char* request)
{
    //打开日志文件
    FILE *fp = fopen(LOG_PATH, "a+");

    //打开失败则结束函数
    if(fp == NULL){
        perror("log filed access failed.");
        return;
    }

    //定义时间变量
    time_t t;
    struct tm *nowtime;
    char timestr[40];

    //获取当前时间
    time(&t);
    nowtime = localtime(&t);
    strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S", nowtime);

    //记录访问的IP地址，时间，以及请求信息
    fprintf(fp, "%s-----------%s--------------%s", inet_ntoa(client_addr->sin_addr), timestr, request);

    //关闭文件流
    fclose(fp);
}
