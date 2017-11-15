#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>

#include <arpa/inet.h>

#include <netinet/in.h>
#include <netinet/ip.h>

#include <sys/types.h>
#include <unistd.h>

#include <pthread.h>
#include <semaphore.h>

#define SERVER_PORT 5015
#define BACKLOG 8

extern int errno;

struct http_request {
	pthread_t tid;
	int sockfd;
	struct sockaddr_in addr;
};

struct request_mesg {
	enum {GET, POST} method;
	union{
		struct {char uri[255];} get;
		struct {char uri[255]; char form[255]; int length;} post;
	} content;
};

struct response_static {
	const char *path;
	const char *type;
	int length;
	int status;
};

struct usr {
	const char *id;
	const char *passwd;
	const char *name;
};

int recv_line(int sockfd, char *recv_buf) {
	//这个函数构造了一个状态机, 该状态机用于每次接收1字符，直到接收到\r\n作为一行的结束
	//返回值 <=0 是 recv() 的错误返回值，返回值 >0 代表接收的这行有多少个字符
	char *p = recv_buf;
	int state = 0;
	int ret;
	
	while (1) {
		ret = (int)recv(sockfd, p, 1, 0);
		
		if (ret <= 0) {
			break;
		}
		switch (state) {
			case 0:
				if (*p == '\r')
					state = 1;
				break;
			case 1:
				if (*p != '\n') {
					state = 0;
				}
				else {
					p++;
					*p = 0;
					state = 2;
					break;
				}
				break;
			default:
				assert(0);
				break;
		}
		
		if (state == 2)
			break;
		
		p++;
	}
	
	if (ret > 0) {
		ret = p - recv_buf;
	}
	return ret;
}

struct response_static uri2path(struct request_mesg *mesg) {
	//该函数作用是从URI到具体文件路径的转换
	struct response_static f;
	f.status = 200;
	
	if (strcmp(mesg->content.get.uri, "/test.html") == 0) {
		f.path = "/home/ubuntu/lab8/html/test.html";
		f.type = "text/html";
	}
	else if (strcmp(mesg->content.get.uri, "/noimg.html") == 0) {
		f.path = "/home/ubuntu/lab8/html/noimg.html";
		f.type = "text/html";
	}
	else if (strcmp(mesg->content.get.uri, "/txt/test.txt") == 0) {
		f.path = "/home/ubuntu/lab8/txt/test.txt";
		f.type = "text/plain";
	}
	else if (strcmp(mesg->content.get.uri, "/img/logo.jpg") == 0) {
		f.path = "/home/ubuntu/lab8/img/logo.jpg";
		f.type = "image/jpeg";
	}
	else {
		f.status = 404;
	}
	
	return f;
}

void fill_body(struct response_static *f, char *buffer) {
	//将文件内容读到buffer中, 并获取文件的长度
	FILE *fp = fopen(f->path, "rb");
	if (fp == NULL) {
		printf("Err: Can't open %s.\n", f->path);
		*buffer = 0;
		f->length = 0;
	} else {
		fseek(fp, 0, SEEK_END);
		f->length = ftell(fp);
		fseek(fp, 0, SEEK_SET);
		fread(buffer, sizeof(char), f->length, fp);
		fclose(fp);
	}
}

void form_parse(char *form, char *usrname, char *passwd) {
	//按照固定格式获取表单中user和password
	*usrname = *passwd = 0;
	char *p = strchr(form, '&');
	*p = 0;
	sscanf(form, "login=%s", usrname);
	sscanf(p+1, "pass=%s", passwd);
}

const char * login_check(char *usrname, char *passwd) {
	//检查用户名和密码是否正确，若正确返回用户名
	static struct usr table[] = {{"3140105015", "5015", "Li Bo"},
		{"3140105013", "5013", "ZHANG Minghan"}};
	int cnt = sizeof(table) / sizeof(struct usr);
	int pid = -1;
	for (int i = 0; i < cnt; i++) {
		if (strcmp(usrname, table[i].id) == 0 && strcmp(passwd, table[i].passwd) == 0) {
			pid = i;
			break;
		}
	}
	if (pid == -1)
		return NULL;
	return table[pid].name;
}

void* thread(void *para) {
	struct http_request *request = (struct http_request *)para;

	struct request_mesg mesg;
	char recv_buf[1024];
	int send_buf_len;
	char send_buf[81920];
	char send_header_buf[1024];
	char send_body_buf[81920];
	
	printf("request: \n");
	
	while (1) {
		//Http头部读取一行内容
		int ret = recv_line(request->sockfd, recv_buf);
		
		if (ret <= 0) {
			printf("In receive(), err = %d\n", errno);
			break;
		}
		else if (ret == 2) {
			//如果读取到一行是0D, 0A代表着http头部结束
			if (mesg.method == POST) {
				ret = recv(request->sockfd, mesg.content.post.form, mesg.content.post.length, 0);
				if (ret <= 0) {
					printf("In receive(), err = %d\n", errno);
				} 
			}
			break;
		}
		
		if (strlen(recv_buf) > 3 && strncmp(recv_buf, "GET", 3) == 0) {
			//解析到该请示是GET, 记录其URI
			mesg.method = GET;
			sscanf(recv_buf + 3, "%s", mesg.content.get.uri);
		}
		else if (strlen(recv_buf) > 4 && strncmp(recv_buf, "POST", 4) == 0) {
			//解析到该请求是POST, 记录其URI
			mesg.method = POST;
			sscanf(recv_buf + 4, "%s", mesg.content.post.uri);
		}
		else if (mesg.method == POST && strlen(recv_buf) > 15 && strncmp(recv_buf, "Content-Length:", 15) == 0) {
			//若该请求是POST, 且头部含有字段Content-Length, 则将这个长度记录下来
			sscanf(recv_buf + 15, "%d", &mesg.content.post.length);
		}
		
		printf("%s", recv_buf);
	}
	
	struct response_static response_file;
	
	//处理请求为GET的情况
	if (mesg.method == GET) {
		//将URL转换为文件路径, 并获取其类型
		response_file = uri2path(&mesg);
		
		if (response_file.status == 200) {
			//读取文件内容
			fill_body(&response_file, send_body_buf);
			
			//构造Http响应头部
			sprintf(send_header_buf, 
					"HTTP/1.1 %d OK\r\nContent-Length: %d\r\nContent-Type: %s\r\n",
					response_file.status,
					response_file.length,
					response_file.type);
			
			//构造Http响应包, 计算传输长度
			sprintf(send_buf, "%s\r\n", send_header_buf);
			memcpy(send_buf + strlen(send_header_buf) + 2, send_body_buf, response_file.length);
			send_buf_len = strlen(send_header_buf) + response_file.length + 2;
		} else {
			//路径错误, 返回404错误
			sprintf(send_buf, "HTTP/1.1 404 Not Found\r\n\r\n");
			send_buf_len = strlen(send_buf);
		}
	}
	
	char usrname[256], password[256];
	char login_response[256];
	const char *login_name;
	
	//处理请求为POST的情况
	if (mesg.method == POST) {
		if (strcmp(mesg.content.post.uri, "/dopost") == 0) {
			//解析表单, 得到用户名和密码
			form_parse(mesg.content.post.form, usrname, password);
			
			//查表, 检查用户名密码是否正确, 返回名字
			login_name = login_check(usrname, password);
			
			//构造Http响应的体部
			if (login_name) {
				sprintf(login_response, "Login success, Welcome %s:", login_name);
			}
			else {
				sprintf(login_response, "login or pass wrong");
			}
			sprintf(send_body_buf, "<html><head><title>login</title></head><body><h1>%s</h1><p>The login you input is.</p><table border=\"1\"><tr><td>user</td><td>%s</td></tr><td>pass</td><td>%s</td></tr></table><h2>other links</h2><a href=\"http://www.ncj.today:5015/test.html\">html/test.html</a><br><a href=\"http://www.ncj.today:5015/noimg.html\">html/noimg.html</a><br><a href=\"http://www.ncj.today:5015/img/logo.jpg\">logo</a><br><a href=\"http://www.ncj.today:5015/txt/test.txt\">txt</a></body></html>", login_response, usrname, password);
			
			//构造Http响应头部
			sprintf(send_header_buf, 
					"HTTP/1.1 %d OK\r\nContent-Length: %ldu\r\nContent-Type: %s\r\n",
					200,
					strlen(send_body_buf),
					"text/html");		
			
			//拼接在一起
			sprintf(send_buf, "%s\r\n%s", send_header_buf, send_body_buf);
			send_buf_len = strlen(send_buf);
		}
		else {
			//POST路径不合法，返回404
			sprintf(send_buf, "HTTP/1.1 404 Not Found\r\n\r\n");
			send_buf_len = strlen(send_buf);			
		}
	}
	
	printf("response: \n%s\n", send_buf);
	printf("#################################################################\n");
	
	//将Http响应数据包发回客户端
	send(request->sockfd, send_buf, send_buf_len, 0);
	
	//释放socket资源
	close(request->sockfd);
	free(request);
	return NULL;
}

int main() {
	int sockfd;
	struct sockaddr_in server_addr;

	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd == -1) {
		printf("In socket(), can't get fd : %d\n", errno);
		return -1;
	}

	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(SERVER_PORT);
	server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(sockfd, (struct sockaddr *) &server_addr, sizeof(struct sockaddr_in)) == -1) {
		printf("In bind(), err = %d \n", errno);
		close(sockfd);
		return -1;
	}

	//开启监听
	if (listen(sockfd, BACKLOG) == -1) {
		printf("In listen(), err = %d\n", errno);
		close(sockfd);
		return -1;
	}
	
	printf("Start to listen client to connect\n");

	while (1) {
		//接受用户连接请求
		struct http_request *request = malloc(sizeof(struct http_request));
		socklen_t sockaddr_size = sizeof(struct sockaddr_in);
		request->sockfd = accept(sockfd, (struct sockaddr *)&(request->addr), &sockaddr_size);
		if (request->sockfd == -1) {
			printf("In accept(), err = %d\n", errno);
			free(request);
			break;
		}

		//创建服务线程并交给其运行
		int ret = pthread_create(&(request->tid), NULL, thread, request);
		if (ret != 0) {
			printf("In pthread_create(), err = %d\n", ret);
			close(request->sockfd);
			free(request);
			break;
		}
		printf("request start\n");
	}

	close(sockfd);

	return 0;
}
