/**********************************************************************************************************************
    > File Name: httpclient.cpp
    > Author: htsong
    > Date: 5/8/24
**********************************************************************************************************************/

#include "httpclient.h"
#include "log.h"
#include <cstdio>

namespace base_tools{

// 文件流式写回调（用于大文件下载）
static size_t WriteToFile(void *buffer, size_t size, size_t nmemb, void *data)
{
	FILE *fp = static_cast<FILE*>(data);
	if (NULL == fp || NULL == buffer)
	{
		return CURLE_WRITE_ERROR;
	}
	return fwrite(buffer, size, nmemb, fp);
}

//cib_atomic_t CHttpClient::aGloNum = 0;
	
CHttpClient::CHttpClient()
{
	bDebug = false;
//	pChunk = NULL;
	//pCurl = NULL;
	eCurlEnv = CHttpClient::GLOBAL_ALL;
	iHtpTmOut = 30;

	curl_global_init(CURL_GLOBAL_ALL);
}
 
CHttpClient::~CHttpClient()
{
//	if( pChunk != NULL )
//		curl_slist_free_all(pChunk);

	curl_global_cleanup();

//	if( cib_atomic_compare_and_swap(aGloNum, 1, 0) )
//		curl_global_cleanup();
//	else
//		cib_atomic_oper(aGloNum, -1);
		
}
/*	
CHttpClient::CHttpClient( int iTmOut, bool bDebug , HTCLEEnv eEnv )
{
	bDebug = false;
	pChunk = NULL;
	pCurl = NULL;
	eCurlEnv = eEnv;
	iHtpTmOut = iTmOut; 
}
*/
void CHttpClient::SetEnvTpye(enum HTCLEEnv eEnv)
{
	eCurlEnv = eEnv;
}

void CHttpClient::SetTimeOut(int iTimOut)
{
	iHtpTmOut = iTimOut;	
}

void CHttpClient::SetDebug(bool bDebugFlag)
{
	bDebug = bDebugFlag;	
}


 
 
size_t CHttpClient::HTCLWriteData(void *buffer, size_t size, size_t nmemb, void *data)
{
	std::string* str = static_cast<std::string*>(data);
	if( NULL == str || NULL == buffer )
	{
		//log
		//CIBLOG(base_tools::CibLog::DEBUG, "WriteData pointer null");
		return CURLE_WRITE_ERROR;
	}
 
	char* pData = (char*)buffer;
	str->append(pData, size * nmemb);
	return size * nmemb;
}
//
//bool CHttpClient::HTCLEnvInit()
//{
//	//CURL_GLOBAL_ALL                      //初始化所有的可能的调用。
//	//CURL_GLOBAL_SSL                      //初始化支持 安全套接字层。
//	//CURL_GLOBAL_WIN32            //初始化win32套接字库。
//	//CURL_GLOBAL_NOTHING         //没有额外的初始化。
//	long flag = CURL_GLOBAL_ALL;
//	if(eCurlEnv == GLOBAL_ALL)
//		flag = CURL_GLOBAL_ALL;
//	else if(eCurlEnv == GLOBAL_SSL)
//		flag = CURL_GLOBAL_SSL;
//	else if(eCurlEnv == GLOBAL_WIN32)
//		flag = CURL_GLOBAL_WIN32;
//	else if(eCurlEnv == GLOBAL_NOTHING)
//		flag = CURL_GLOBAL_NOTHING;
//	if(cib_atomic_compare_and_swap(aGloNum, 0, 1)){
//		res = curl_global_init(flag);
//    	if(CURLE_OK != res){
//        	std::cout<<"curl curl_global_init failed"<<std::endl;
//        	std::cout<<curl_easy_strerror((CURLcode)res)<<std::endl;
//        	HTCLSetErrInfo();
//        	return false;
//    	}
//	}else
//	cib_atomic_oper(aGloNum, 1);
//
//
//    return true;
//}
#if 0
void CHttpClient::HTCLSetHeader(const std::string& strHeaders)
{
	//std::cout<<"set head"<< strHeaders<<std::endl;
	pChunk = curl_slist_append(pChunk, strHeaders.c_str());
}

void CHttpClient::HTCLSetHeader(const char *pHeaders)
{
	HTCLSetHeader(std::string(pHeaders));
}	
 
void CHttpClient::HTCLClearHeader()
{
	 //std::cout<<"Clear http head"<<std::endl;
	 if(pChunk != NULL)
	 	curl_slist_free_all(pChunk);	
	 //std::cout<<"Clear http head over"<<std::endl;
	 pChunk = NULL;
}
 
bool CHttpClient::HTCLPost(const std::string& strUrl, const std::string& strPostPara, std::string& strRes )
{
	CURL *pCurl = curl_easy_init();
    if( NULL == pCurl)
	{
    	res = CURLE_FAILED_INIT;
        std::cout<<"Init curl_easy_init failed"<<std::endl;
        HTCLSetErrInfo();
        return false;
    }
	if( strUrl.empty() )
	{
		res = base_tools::CErrorInfoParser::HTTP_URL_NULL;
		HTCLSetErrInfo();	
		return false;
	}

	if(bDebug)
	{
		curl_easy_setopt(pCurl, CURLOPT_VERBOSE, 1);
		curl_easy_setopt(pCurl, CURLOPT_DEBUGFUNCTION, Debug);
	}
	curl_easy_setopt(pCurl, CURLOPT_URL, strUrl.c_str());
	
	if(pChunk != NULL)
		curl_easy_setopt(pCurl, CURLOPT_HTTPHEADER, pChunk);
		
	curl_easy_setopt(pCurl, CURLOPT_CUSTOMREQUEST, "POST");
	
	if(!strPostPara.empty())
		curl_easy_setopt(pCurl, CURLOPT_POSTFIELDS, strPostPara.c_str());
	
	curl_easy_setopt(pCurl, CURLOPT_READFUNCTION, NULL);
	curl_easy_setopt(pCurl, CURLOPT_WRITEFUNCTION, HTCLWriteData);
	curl_easy_setopt(pCurl, CURLOPT_WRITEDATA, (void *)&strRes);
	curl_easy_setopt(pCurl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(pCurl, CURLOPT_CONNECTTIMEOUT, iHtpTmOut); // 是链接超时  
	curl_easy_setopt(pCurl, CURLOPT_TIMEOUT, iHtpTmOut); //接受超时
	
	res = curl_easy_perform(pCurl);
	//CIBLOG(base_tools::CibLog::DEBUG, "curl res: %d", res);
	if(CURLE_OK != res)
	{
		//std::cout<<"11::"<<curl_easy_strerror((CURLcode)res)<<std::endl;
		HTCLSetErrInfo();
		curl_easy_cleanup(pCurl); 
		return false;
	}
	curl_easy_cleanup(pCurl);
	return true;
}

bool CHttpClient::HTCLGet(const std::string & strUrl, std::string & strResponse)
{
	CURL *pCurl = curl_easy_init();
	if( NULL == pCurl)
	{
		res = CURLE_FAILED_INIT;
		std::cout<<"Init curl_easy_init failed"<<std::endl;
		HTCLSetErrInfo();
		return false;
	}
	
	if(bDebug)
	{
		curl_easy_setopt(pCurl, CURLOPT_VERBOSE, 1);
		curl_easy_setopt(pCurl, CURLOPT_DEBUGFUNCTION, Debug);
	}
	
	if(pChunk != NULL)
		curl_easy_setopt(pCurl, CURLOPT_HTTPHEADER, pChunk);
	
	std::cout<<"int to  HTCLGet"<<std::endl;
	curl_easy_setopt(pCurl, CURLOPT_CUSTOMREQUEST, "GET");
	curl_easy_setopt(pCurl, CURLOPT_URL, strUrl.c_str());
	curl_easy_setopt(pCurl, CURLOPT_READFUNCTION, NULL);
	curl_easy_setopt(pCurl, CURLOPT_WRITEFUNCTION, HTCLWriteData);
	curl_easy_setopt(pCurl, CURLOPT_WRITEDATA, (void *)&strResponse);
	/**
	* 当多个线程都使用超时处理的时候，同时主线程中有sleep或是wait等操作。
	* 如果不设置这个选项，libcurl将会发信号打断这个wait从而导致程序退出。
	*/
	curl_easy_setopt(pCurl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(pCurl, CURLOPT_CONNECTTIMEOUT, iHtpTmOut);
	curl_easy_setopt(pCurl, CURLOPT_TIMEOUT, iHtpTmOut);
	res = curl_easy_perform(pCurl);
	//CIBLOG(base_tools::CibLog::DEBUG, "curl res: %d", res);
	if(CURLE_OK != res)
	{
		std::cout<<curl_easy_strerror((CURLcode)res)<<std::endl;
		HTCLSetErrInfo();
		curl_easy_cleanup(pCurl); 
		return false;
	}
	curl_easy_cleanup(pCurl); 
	return true;
}
#endif

int CHttpClient::Debug(CURL *, curl_infotype itype, char * pData, size_t size, void *)
{
	if(itype == CURLINFO_TEXT)
	{
		//printf("[TEXT]%s\n", pData);
	}
	else if(itype == CURLINFO_HEADER_IN)
	{
		std::cout<<"[HEADER_IN]:"<<pData<<std::endl;
	}
	else if(itype == CURLINFO_HEADER_OUT)
	{
		std::cout<<"[HEADER_OUT]:"<<pData<<std::endl;
	}
	else if(itype == CURLINFO_DATA_IN)
	{
		std::cout<<"[DATA_IN]:"<<pData<<std::endl;
	}
	else if(itype == CURLINFO_DATA_OUT)
	{
		std::cout<<"[DATA_OUT]:"<<pData<<std::endl;
	}
	return 0;
}

void CHttpClient::HTCLSetErrInfo(int res)
{
	//std::cout<<"into HTCLSetErrInfo"<<std::endl;
		
	cErrParse.SetModType(base_tools::CErrorInfoParser::HTPCL_MOD);
		
	if(CURLE_FAILED_INIT == res){
		cErrParse.SetCode(base_tools::CErrorInfoParser::HTTP_FAILED_INIT);
		cErrParse.SetMess("Http client env init failed");
	}
	else if(CURLE_OPERATION_TIMEDOUT == res){
		cErrParse.SetCode(base_tools::CErrorInfoParser::HTTP_TIME_OUT);
		cErrParse.SetMess("Connection timed out");
		//std::cout<<"err code "<<cErrParse.GetCode() <<std::endl;
	}
	else if(base_tools::CErrorInfoParser::HTTP_URL_NULL == res)
	{
		cErrParse.SetCode(base_tools::CErrorInfoParser::HTTP_URL_NULL);
		cErrParse.SetMess("Url is null");	
	}
	else{
		cErrParse.SetCode(base_tools::CErrorInfoParser::HTTP_COM_EXCEPT);
		cErrParse.SetMess("Exceptional communication with server");
	}
	//std::cout<<"out HTCLSetErrInfo"<<std::endl;
	return ;
}

int CHttpClient::GetErrCode()
{
	return cErrParse.GetCode();
}	
std::string CHttpClient::GetErrMess()
{
	return cErrParse.GetMessage();
}

#if 0
bool CHttpClient::HTCLPosts(const std::string & strUrl, const std::string & strPost, std::string & strResponse, const char * pCaPath)  
{
    CURLcode res;  
    CURL* curl = curl_easy_init();  
    if(NULL == curl)  
    {  
        return CURLE_FAILED_INIT;  
    }  
    if(bDebug)  
    {  
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);  
        curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, Debug);  
    }  
    curl_easy_setopt(curl, CURLOPT_URL, strUrl.c_str());  

	if(pChunk != NULL)
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, pChunk);
	
    curl_easy_setopt(curl, CURLOPT_POST, 1);  
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, strPost.c_str());  
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, NULL);  
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, HTCLWriteData);  
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&strResponse);  
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if(NULL == pCaPath)  
    {  
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, false);  
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, false);  
    }  
    else  
    {  
        //缺省情况就是PEM，所以无需设置，另外支持DER  
        //curl_easy_setopt(curl,CURLOPT_SSLCERTTYPE,"PEM");  
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, true);  
        curl_easy_setopt(curl, CURLOPT_CAINFO, pCaPath);  
    }  
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, iHtpTmOut);  
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, iHtpTmOut);  
    res = curl_easy_perform(curl);
	//CIBLOG(base_tools::CibLog::DEBUG, "curl res: %d", res);
	if(CURLE_OK != res)
	{
		std::cout<<curl_easy_strerror((CURLcode)res)<<std::endl;
		HTCLSetErrInfo();
		curl_easy_cleanup(curl); 
		return false;
	}
	curl_easy_cleanup(curl); 
    return true;  
}  
  
bool CHttpClient::HTCLGets(const std::string & strUrl, std::string & strResponse, const char * pCaPath)  
{  
//	std::lock_guard<std::mutex> aLock(mMutex);
	CURLcode res;
    CURL* curl = curl_easy_init();  
    if(NULL == curl)  
    {  
        return CURLE_FAILED_INIT;  
    }  
    if(bDebug)  
    {  
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);  
        curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, Debug);  
    }  
    curl_easy_setopt(curl, CURLOPT_URL, strUrl.c_str());  

	if(pChunk != NULL)
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, pChunk);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, NULL);  
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, HTCLWriteData);  
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&strResponse);  
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if(NULL == pCaPath)  
    {  
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, false);  
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, false);  
    }  
    else  
    {  
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, true);  
        curl_easy_setopt(curl, CURLOPT_CAINFO, pCaPath);  
    }  
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, iHtpTmOut);  
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, iHtpTmOut);  
    res = curl_easy_perform(curl);
	//CIBLOG(base_tools::CibLog::DEBUG, "curl res: %d", res);
	if(CURLE_OK != res){
		std::cout<<curl_easy_strerror((CURLcode)res)<<std::endl;
		HTCLSetErrInfo();
		curl_easy_cleanup(curl); 
		return false;
	}
	curl_easy_cleanup(curl); 
    return true;  
}  
#endif

//--------------------------------------------------------
bool CHttpClient::HTCLGet(const std::string & strUrl, const std::vector<std::string>& headers, std::string & strResponse)
{
	int res;
	CURL *pCurl = curl_easy_init();
	if( NULL == pCurl)
	{
		res = CURLE_FAILED_INIT;
		std::cout<<"Init curl_easy_init failed"<<std::endl;
		HTCLSetErrInfo(res);
		return false;
	}

	if(bDebug)
	{
		curl_easy_setopt(pCurl, CURLOPT_VERBOSE, 1);
		curl_easy_setopt(pCurl, CURLOPT_DEBUGFUNCTION, Debug);
	}

	struct curl_slist *pChunk = NULL;
	for (const auto& item : headers)
	{
		auto temp = curl_slist_append(pChunk, item.c_str());
		if (temp)
		{
			pChunk = temp;
		}
	}
	if(pChunk != NULL)
		curl_easy_setopt(pCurl, CURLOPT_HTTPHEADER, pChunk);

	curl_easy_setopt(pCurl, CURLOPT_CUSTOMREQUEST, "GET");
	curl_easy_setopt(pCurl, CURLOPT_URL, strUrl.c_str());
	curl_easy_setopt(pCurl, CURLOPT_READFUNCTION, NULL);
	curl_easy_setopt(pCurl, CURLOPT_WRITEFUNCTION, HTCLWriteData);
	curl_easy_setopt(pCurl, CURLOPT_WRITEDATA, (void *)&strResponse);
	/**
	* 当多个线程都使用超时处理的时候，同时主线程中有sleep或是wait等操作。
	* 如果不设置这个选项，libcurl将会发信号打断这个wait从而导致程序退出。
	*/
	curl_easy_setopt(pCurl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(pCurl, CURLOPT_CONNECTTIMEOUT, iHtpTmOut);
	curl_easy_setopt(pCurl, CURLOPT_TIMEOUT, iHtpTmOut);
	res = curl_easy_perform(pCurl);
	//CIBLOG(base_tools::CibLog::DEBUG, "curl res: %d", res);

	if(pChunk != NULL)
		curl_slist_free_all(pChunk); /* free the header list */
	pChunk = NULL;

	if(CURLE_OK != res)
	{
		std::cout<<curl_easy_strerror((CURLcode)res)<<std::endl;
		HTCLSetErrInfo(res);
		curl_easy_cleanup(pCurl);
		return false;
	}
	curl_easy_cleanup(pCurl);
	return true;
}

bool CHttpClient::HTCLPost(const std::string& strUrl, const std::string& strPostPara, const std::vector<std::string>& headers, std::string& strRes )
{
	int res;
	CURL *pCurl = curl_easy_init();
    if( NULL == pCurl)
	{
    	res = CURLE_FAILED_INIT;
        LogWarn << "Init curl_easy_init failed";
        HTCLSetErrInfo(res);
        return false;
    }
	if( strUrl.empty() )
	{
		res = base_tools::CErrorInfoParser::HTTP_URL_NULL;
		HTCLSetErrInfo(res);
		return false;
	}
	if(bDebug)
	{
		curl_easy_setopt(pCurl, CURLOPT_VERBOSE, 1);
		curl_easy_setopt(pCurl, CURLOPT_DEBUGFUNCTION, Debug);
	}
	curl_easy_setopt(pCurl, CURLOPT_URL, strUrl.c_str());
	struct curl_slist *pChunk = NULL;
	for (const auto& item : headers)
	{
		auto temp = curl_slist_append(pChunk, item.c_str());
		if (temp)
		{
			pChunk = temp;
		}
	}
	if(pChunk != NULL)
		curl_easy_setopt(pCurl, CURLOPT_HTTPHEADER, pChunk);

	curl_easy_setopt(pCurl, CURLOPT_CUSTOMREQUEST, "POST");
	if(!strPostPara.empty())
		curl_easy_setopt(pCurl, CURLOPT_POSTFIELDS, strPostPara.c_str());

	curl_easy_setopt(pCurl, CURLOPT_READFUNCTION, NULL);
	curl_easy_setopt(pCurl, CURLOPT_WRITEFUNCTION, HTCLWriteData);
	curl_easy_setopt(pCurl, CURLOPT_WRITEDATA, (void *)&strRes);
	curl_easy_setopt(pCurl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(pCurl, CURLOPT_CONNECTTIMEOUT, iHtpTmOut); // 是链接超时
	curl_easy_setopt(pCurl, CURLOPT_TIMEOUT, iHtpTmOut); //接受超时
	res = curl_easy_perform(pCurl);
	//CIBLOG(base_tools::CibLog::DEBUG, "curl res: %d", res);
	if(pChunk != NULL)
		curl_slist_free_all(pChunk); /* free the header list */
	pChunk = NULL;
	if(CURLE_OK != res)
	{
        LogWarn << "11::" << curl_easy_strerror((CURLcode)res) << " ,res: " << res ;
		HTCLSetErrInfo(res);
		curl_easy_cleanup(pCurl);
		return false;
	}
	curl_easy_cleanup(pCurl);
	return true;
}

bool CHttpClient::HTCLPosts(const std::string & strUrl, const std::string & strPost, const std::vector<std::string>& headers, std::string & strResponse, const char * pCaPath)
{
    CURLcode res;
    CURL* curl = curl_easy_init();
    if(NULL == curl)
    {
        return CURLE_FAILED_INIT;
    }
    if(bDebug)
    {
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);
        curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, Debug);
    }
    curl_easy_setopt(curl, CURLOPT_URL, strUrl.c_str());

	struct curl_slist *pChunk = NULL;
	for (const auto& item : headers)
	{
		auto temp = curl_slist_append(pChunk, item.c_str());
		if (temp)
		{
			pChunk = temp;
		}
	}
	if(pChunk != NULL)
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, pChunk);

    curl_easy_setopt(curl, CURLOPT_POST, 1);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, strPost.c_str());
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, NULL);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, HTCLWriteData);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&strResponse);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if(NULL == pCaPath)
    {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, false);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, false);
    }
    else
    {
        //缺省情况就是PEM，所以无需设置，另外支持DER
        //curl_easy_setopt(curl,CURLOPT_SSLCERTTYPE,"PEM");
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, true);
        curl_easy_setopt(curl, CURLOPT_CAINFO, pCaPath);
    }
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, iHtpTmOut);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, iHtpTmOut);
    res = curl_easy_perform(curl);
	//CIBLOG(base_tools::CibLog::DEBUG, "curl res: %d", res);

	if(pChunk != NULL)
		curl_slist_free_all(pChunk); /* free the header list */
	pChunk = NULL;

	if(CURLE_OK != res)
	{
		std::cout<<curl_easy_strerror((CURLcode)res)<<std::endl;
		HTCLSetErrInfo(res);
		curl_easy_cleanup(curl);
		return false;
	}
	curl_easy_cleanup(curl);
	//CIBLOG(base_tools::CibLog::DEBUG, "here before return true");
    return true;
}

bool CHttpClient::HTCLGets(const std::string & strUrl, const std::vector<std::string>& headers, std::string & strResponse, const char * pCaPath)
{
//	std::lock_guard<std::mutex> aLock(mMutex);
	CURLcode res;
    CURL* curl = curl_easy_init();
    if(NULL == curl)
    {
        return CURLE_FAILED_INIT;
    }
    if(bDebug)
    {
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);
        curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, Debug);
    }
    curl_easy_setopt(curl, CURLOPT_URL, strUrl.c_str());

	struct curl_slist *pChunk = NULL;
	for (const auto& item : headers)
	{
		auto temp = curl_slist_append(pChunk, item.c_str());
		if (temp)
		{
			pChunk = temp;
		}
	}
	if(pChunk != NULL)
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, pChunk);

	curl_easy_setopt(curl, CURLOPT_READFUNCTION, NULL);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, HTCLWriteData);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&strResponse);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if(NULL == pCaPath)
    {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, false);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, false);
    }
    else
    {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, true);
        curl_easy_setopt(curl, CURLOPT_CAINFO, pCaPath);
    }
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, iHtpTmOut);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, iHtpTmOut);
    res = curl_easy_perform(curl);
	//CIBLOG(base_tools::CibLog::DEBUG, "curl res: %d", res);

	if(pChunk != NULL)
		curl_slist_free_all(pChunk); /* free the header list */
	pChunk = NULL;

	if(CURLE_OK != res){
		std::cout<<curl_easy_strerror((CURLcode)res)<<std::endl;
		HTCLSetErrInfo(res);
		curl_easy_cleanup(curl);
		return false;
	}
	curl_easy_cleanup(curl);
    return true;
}

bool CHttpClient::HTCLDownloadFile(const std::string & strUrl, const std::string & strFilePath, const std::vector<std::string>& headers, const char * pCaPath)
{
	CURLcode res;
	CURL* curl = curl_easy_init();
	if(NULL == curl)
	{
		return false;
	}
	FILE* fp = fopen(strFilePath.c_str(), "wb");
	if(NULL == fp)
	{
		curl_easy_cleanup(curl);
		return false;
	}

	if(bDebug)
	{
		curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);
		curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, Debug);
	}
	curl_easy_setopt(curl, CURLOPT_URL, strUrl.c_str());

	struct curl_slist *pChunk = NULL;
	for (const auto& item : headers)
	{
		auto temp = curl_slist_append(pChunk, item.c_str());
		if (temp)
		{
			pChunk = temp;
		}
	}
	if(pChunk != NULL)
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, pChunk);

	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteToFile);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)fp);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);   // 允许重定向
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);  // 连接超时 30s
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);          // 不限总时长，大文件下载
	if(NULL == pCaPath)
	{
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, false);
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, false);
	}
	else
	{
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, true);
		curl_easy_setopt(curl, CURLOPT_CAINFO, pCaPath);
	}
	res = curl_easy_perform(curl);

	fflush(fp);
	fclose(fp);

	if(pChunk != NULL)
		curl_slist_free_all(pChunk);

	if(CURLE_OK != res)
	{
		std::cout<<curl_easy_strerror((CURLcode)res)<<std::endl;
		HTCLSetErrInfo(res);
		curl_easy_cleanup(curl);
		std::remove(strFilePath.c_str());   // 下载失败清理残文件
		return false;
	}
	curl_easy_cleanup(curl);
	return true;
}
///////////////////////////////////////////////////////////////////////////////////////////////

} //namespace base_tools
