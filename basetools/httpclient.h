/**********************************************************************************************************************
    > File Name: httpclient.cpp
    > Author: htsong
    > Date: 5/8/24
**********************************************************************************************************************/
#ifndef BASE_TOOLS_HTTP_CLIENT_H
#define BASE_TOOLS_HTTP_CLIENT_H
#include <iostream>
#include <string>
#include <curl/curl.h>
#include "base_error_info.h"
#include <mutex>
#include <vector>

namespace base_tools
{
//class  CErrorInfoParser
class CHttpClient
{
public:
	enum HTCLEEnv
	{
		GLOBAL_ALL,
		GLOBAL_SSL,
		GLOBAL_WIN32,
		GLOBAL_NOTHING
	};

private:
	//CHttpClient( int iTmOut = 5, bool bDebug = false, HTCLEEnv eEnv = CHttpClient::GLOBAL_ALL );
	CHttpClient();
	~CHttpClient();
    CHttpClient(const CHttpClient&) = delete;
    CHttpClient& operator=(const CHttpClient&) = delete;

public:
	static CHttpClient& getInstance()
    {
        static CHttpClient instance; //using static feature
        return instance;
    }
 
public:
	
//	bool HTCLEnvInit();
	
	void SetDebug(bool bDebugFlag);
		
	void SetTimeOut(int iTimOut);
		
	void SetEnvTpye(enum HTCLEEnv eEnv);
	
	/**
	* @brief HTTP head
	* @param strHeaders ,如: "Content-Type: application/json"
	* @return 返回是否Post成功
	*/
	void HTCLSetHeader(const std::string& strHeaders);
	
	void HTCLSetHeader(const char *pHeaders);		
	/**
	* @brief HTTP POST请求
	* @param strUrl 输入参数,请求的Url地址,如:http://www.baidu.com
	* @param strPost 输入参数,使用如下格式
	* @param strResponse 输出参数,返回的内容
	* @return 返回是否Post成功
	*/
//	bool HTCLPost(const std::string& strUrl, const std::string& strPostPara, std::string& strRes );
	bool HTCLPost(const std::string& strUrl, const std::string& strPostPara, const std::vector<std::string>& headers, std::string& strRes );
	
//	bool HTCLGet(const std::string & strUrl, std::string & strResponse);
	bool HTCLGet(const std::string & strUrl, const std::vector<std::string>& headers, std::string & strResponse);
 
	//int Posts(const std::string & strUrl, const std::string & strPost, std::string & strResponse, const char * pCaPath = NULL);
 
	//int Gets(const std::string & strUrl, std::string & strResponse, const char * pCaPath = NULL);




	/** 
		* @brief HTTPS POST请求,无证书版本 
		* @param strUrl 输入参数,请求的Url地址,如:https://www.alipay.com 
		* @param strPost 输入参数,使用如下格式para1=val1?2=val2&… 
		* @param strResponse 输出参数,返回的内容 
		* @param pCaPath 输入参数,为CA证书的路径.如果输入为NULL,则不验证服务器端证书的有效性. 
		* @return 返回是否Post成功 
		*/	
//	bool HTCLPosts(const std::string & strUrl, const std::string & strPost, std::string & strResponse, const char * pCaPath = NULL);
	bool HTCLPosts(const std::string & strUrl, const std::string & strPost, const std::vector<std::string>& headers, std::string & strResponse, const char * pCaPath = NULL);
	  
		/** 
		* @brief HTTPS GET请求,无证书版本 
		* @param strUrl 输入参数,请求的Url地址,如:https://www.alipay.com 
		* @param strResponse 输出参数,返回的内容 
		* @param pCaPath 输入参数,为CA证书的路径.如果输入为NULL,则不验证服务器端证书的有效性. 
		* @return 返回是否Post成功 
		*/	
//	bool HTCLGets(const std::string & strUrl, std::string & strResponse, const char * pCaPath = NULL);
	bool HTCLGets(const std::string & strUrl, const std::vector<std::string>& headers, std::string & strResponse, const char * pCaPath = NULL);

	/**
	* @brief HTTP(S) 下载文件（流式写盘，适合大文件如固件）
	* @param strUrl 下载地址
	* @param strFilePath 保存到本地的文件路径
	* @param headers 请求头
	* @param pCaPath CA 证书路径，NULL 则不校验服务端证书
	* @return 返回是否下载成功
	*/
	bool HTCLDownloadFile(const std::string & strUrl, const std::string & strFilePath, const std::vector<std::string>& headers, const char * pCaPath = NULL);

	void HTCLClearHeader();
	
	int GetErrCode();
	std::string GetErrMess();
	
private:
	
	void HTCLSetErrInfo(int res);
	static size_t HTCLWriteData(void *buffer, size_t size, size_t nmemb, void *data);
	static int Debug(CURL *, curl_infotype itype, char * pData, size_t size, void *);
	
	
private:
    base_tools::CErrorInfoParser cErrParse;
	bool bDebug;
	//CURL *pCurl;
//	struct curl_slist *pChunk;
	enum HTCLEEnv eCurlEnv;
	int iHtpTmOut;
//	int res;

//	std::mutex mMutex;
};



}
#endif
