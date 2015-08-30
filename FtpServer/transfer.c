#include "transfer.h"
#include "common.h"
#include "ftp_command.h"
#include "assist.h"
#include "ftp_process.h"
#include "ftp_channel.h"

static int ftp_get_data_fd(ftp_event_t *ptr);
static char* ftp_get_list_name(struct stat *buf,const char* name);
static char* ftp_get_list_size(struct stat *buf);
static char* ftp_get_list_info(struct stat *buf);
static char* ftp_get_list_type(struct stat *buf);
static char* ftp_get_list_time(struct stat *buf);

void download_file(ftp_event_t *ptr)
{
	//打开文件
	int fd = open(ftp_connection.args,O_RDONLY);
	if(fd <  0){
		ftp_reply(ptr,FTP_FILE_FAIL,"Open file fail\r\n");
		return ;
	}

	//对文件加读锁
	if(FileReadLock(fd) < 0){
		ftp_reply(ptr,FTP_FILE_FAIL,"Open file fail\r\n");
		return ;
	}
	struct stat file;
	if(fstat(fd,&file) < 0){
		FileUnlock(fd);
		ftp_reply(ptr,FTP_FILE_FAIL,"Open file fail\r\n");
		return ;
	}

	//只能传递普通文件
	if(!S_ISREG(file.st_mode)){
		FileUnlock(fd);
		ftp_reply(ptr,FTP_FILE_FAIL,"Can only download regular file\r\n");
		return ;
	}

	//建立数据连接
	if(ftp_get_data_fd(ptr) < 0){
		FileUnlock(fd);
		return ;
	}

	size_t filesize = file.st_size;
	filesize -= ftp_connection.restart_pos;
	lseek(fd,ftp_connection.restart_pos,SEEK_SET);
	ftp_connection.restart_pos = 0;

	char text[64];
	if(ftp_connection.transmode == 0){
		sprintf(text,"Begin to transfer the file in ASCII mode(%d bytes)",filesize);
	}
	else
		sprintf(text,"Begin to transfer the file in BINARY mode(%d bytes)",filesize);
	ftp_reply(ptr,FTP_DATA_OK,text);


	while(filesize > 0){
		size_t nwrite = sendfile(ptr->fd,fd,NULL,65536);
		if(nwrite == -1 && errno == EINTR)	continue;
		filesize -= nwrite;
	}

	FileUnlock(fd);
	close(fd);
	close(ptr->fd);
	ftp_reply(ptr,FTP_DATA_OVER_CLOSE,"Download file successfully\r\n");

}

void upload_file(ftp_event_t *ptr,int op)
{
	//打开或创建文件
	int fd = open(ftp_connection.args,O_WRONLY | O_CREAT,0666);
	if(fd < 0){
		ftp_reply(ptr,FTP_FILE_FAIL,"Upload file fail");
		return ;
	}

	//读锁
	if(FileWriteLock(fd) < 0){
		ftp_reply(ptr,FTP_FILE_FAIL,"Upload file fail");
		return ;
	}

	//APPE
	if(op == 0){
		lseek(fd,0,SEEK_END);
	}

	//STOR
	else{
		ftruncate(fd,ftp_connection.restart_pos);
		lseek(fd,ftp_connection.restart_pos,SEEK_SET);
	}

	if(ftp_get_data_fd(ptr) < 0){
		return ;
	}

	ftp_reply(ptr,FTP_DATA_OK,"Data connection founded");
	char buf[65536];
	while(1){

		int nsize = Read(ptr->fd,buf,sizeof(buf));
		if(nsize == 0)	break;
		Write(fd,buf,nsize);

	}
	FileUnlock(fd);
	close(fd);
	close(ptr->fd);
	ftp_reply(ptr,FTP_DATA_OVER_CLOSE,"Upload file successfully\r\n");

}

void send_list(ftp_event_t *ptr,int op)
{
	if(ftp_get_data_fd(ptr) < 0){
		return ;
	}
    DIR *dir = opendir(".");
	if(dir == NULL){
		ftp_reply(ptr,FTP_COMMAND_FAIL,"Can not open directory");
		return ;
	}
	ftp_reply(ptr,FTP_DATA_OK,"Here comes the directory list");

	//LIST方式
	if(op == 0){

#ifdef TEST
	printf("Enter LIST option\n");
#endif
		struct dirent *dp;
		while(dp = readdir(dir)){
			if(dp->d_name[0] == '.')		continue;
			char text[1024] = {0};
			struct stat buf;
			if(lstat(dp->d_name,&buf) < 0)
					err_quit("SendList - lstat");
			strcpy(text,ftp_get_list_type(&buf));
			strcat(text," ");
			strcat(text,ftp_get_list_info(&buf));
			strcat(text," ");
			strcat(text,ftp_get_list_size(&buf));
			strcat(text," ");
			strcat(text,ftp_get_list_time(&buf));
			strcat(text," ");
			strcat(text,ftp_get_list_name(&buf,dp->d_name));
			strcat(text,"\r\n");
#ifdef TEST
	printf("%s",text);
#endif
			Write(ptr->fd,text,strlen(text));
		}

	}

	//NLST方式
	else{

#ifdef TEST
	printf("Enter NLST option");
#endif
		struct dirent *dp;
	    while(dp = readdir(dir)){
			if(dp->d_name[0] == '.')		continue;
	   		char text[1024] = {0};
	   		struct stat buf;
		    if(lstat(dp->d_name,&buf) < 0)
					err_quit("SendList - lstat");
			strcpy(text,ftp_get_list_name(&buf,dp->d_name));
			strcat(text,"\r\n");
#ifdef TEST
	printf("%s",text);
#endif
			Write(ptr->fd,text,strlen(text));
	    }
	}

	closedir(dir);
	close(ptr->fd);
	ftp_reply(ptr,FTP_DATA_OVER_CLOSE,"Send directory completely");
}



static int ftp_get_data_fd(ftp_event_t *ptr)
{
	if(!ftp_connection.pasv && !ftp_connection.port){
		ftp_reply(ptr,FTP_COMMAND_FAIL,"Please designated the work mode(PASV/PORT)\r\n");
		return FTP_ERROR;
	}

	//PASV模式
	if(ftp_connection.pasv){
		ftp_ipc_send_msg(ftp_connection.nobodyfd,IPC_ACCEPT,-1);
		char res;
		IpcRecvResult(ptr,&res);
		if(res != IPC_COMMAND_OK){
			ftp_reply(ptr,FTP_DATA_BAD,"Can not found data connection\r\n");
			return -1;
		}
		if(IpcRecvFd(ptr,&ptr->fd) < 0){
			ftp_reply(ptr,FTP_DATA_BAD,"Can not found data connection\r\n");
			return -1;
		}
	}

	//PORT模式
	else{
		ftp_ipc_send_msg(ftp_connection.nobodyfd,IPC_CONNECT,-1);
		ftp_ipc_send_msg(ftp_connection.nobodyfd,ftp_connection.addr->sin_addr.s_addr,-1);
        ftp_ipc_send_msg(ftp_connection.nobodyfd,ftp_connection.addr->sin_port,-1);

        int msg;
        int datafd;
		ftp_ipc_recv_msg(ftp_connection.nobodyfd,&msg,datafd);
		if(msg == IPC_COMMAND_BAD){
			ftp_reply(ptr,FTP_DATA_BAD,"Can not found data connection\r\n");
			return -1;
		}
	}

	ftp_connection.pasv = 0;
	if(ftp_connection.port){
		ftp_connection.port = 0;
		free(ftp_connection.addr);
	}

	return 0;
}


static char* ftp_get_list_name(struct stat *pstat,const char *name)
{
	static char filename[128] = {0};
	if(S_ISLNK(pstat->st_mode)){
		char buf[64];
		readlink(name,buf,sizeof(buf));
		sprintf(filename,"%s -> %s",name,buf);
	}
	else
		strcpy(filename,name);
	return filename;
}

static char* ftp_get_list_type(struct stat *pstat)
{
	static char filetype[] = "----------";

	if(S_ISREG(pstat->st_mode)) 		filetype[0] = '-';
	else if(S_ISDIR(pstat->st_mode))	filetype[0] = 'd';
	else if(S_ISCHR(pstat->st_mode))	filetype[0] = 'c';
	else if(S_ISBLK(pstat->st_mode))	filetype[0] = 'b';
	else if(S_ISFIFO(pstat->st_mode))	filetype[0] = 'p';
	else if(S_ISLNK(pstat->st_mode))	filetype[0] = 'l';
	else if(S_ISSOCK(pstat->st_mode))	filetype[0] = 's';

	filetype[1] = (pstat->st_mode & S_IRUSR) ? 'r' : '-';
	filetype[2] = (pstat->st_mode & S_IWUSR) ? 'w' : '-';
	filetype[3] = (pstat->st_mode & S_IXUSR) ? 'x' : '-';

	filetype[4] = (pstat->st_mode & S_IRGRP) ? 'r' : '-';
	filetype[5] = (pstat->st_mode & S_IWGRP) ? 'w' : '-';
	filetype[6] = (pstat->st_mode & S_IXGRP) ? 'x' : '-';

	filetype[7] = (pstat->st_mode & S_IROTH) ? 'r' : '-';
	filetype[8] = (pstat->st_mode & S_IWOTH) ? 'w' : '-';
	filetype[9] = (pstat->st_mode & S_IXOTH) ? 'x' : '-';

	return filetype;
}

static char* ftp_get_list_size(struct stat *pstat)
{
	static char filesize[16];
	sprintf(filesize,"%8u",(unsigned)pstat->st_size);
	return filesize;

}

static char* ftp_get_list_info(struct stat *pstat)
{
	static char fileinfo[128];
	sprintf(fileinfo," %3d %8d %8d",pstat->st_nlink,pstat->st_uid,pstat->st_gid);
	return fileinfo;
}

static char* ftp_get_list_time(struct stat *pstat)
{
	static char filetime[128];
	time_t tsec = pstat->st_ctime;
	struct tm *p = localtime(&tsec);
	if(p == NULL){
		err_quit("GetListTime - localtime");
	}
	strftime(filetime,sizeof(filetime),"%b %e %H:%M",p);
	return filetime;
}







