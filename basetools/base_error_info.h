//
// Created by htsong on 2023/12/06.
//
#ifndef __CIB_ERROR_INFO_H__
#define __CIB_ERROR_INFO_H__
#include <string>

namespace base_tools
{
	
	
class  CErrorInfoParser
{
public:
	enum EModType
	{
		HTPCL_MOD = 1,
		JSON_MOD
	};
	enum EErrType
	{
		//1001  需要跟杭研划分 code段
		HTTP_FAILED_INIT = 1001,
		HTTP_TIME_OUT,
		HTTP_COM_EXCEPT,        //http通讯异常
		HTTP_URL_NULL,       
		//2001
		JSON_PARSE_ERR = 2001
	};
public:
	CErrorInfoParser();
	~CErrorInfoParser();
	
	void SetModType(EModType e );
	
	void SetCode(int iCode);
	
	void SetMess(std::string& strMes);
	
	void SetMess(const char *pMes);
	
	int GetCode();
	
	std::string GetMessage();
	
private:	
	int iModType;
	int iErrCode;
	std::string strErrMess;	
	EModType  eModType;
};


} //namespace base_tools

#endif
