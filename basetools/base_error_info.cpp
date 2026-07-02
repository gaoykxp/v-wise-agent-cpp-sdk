
#include "base_error_info.h"

namespace base_tools
{
	
CErrorInfoParser::CErrorInfoParser()
{
}

CErrorInfoParser::~CErrorInfoParser()
{
	
}			

void CErrorInfoParser::SetModType( EModType e )
{
	eModType = e;
}
	
void CErrorInfoParser::SetCode(int iCode)
{
		iErrCode = iCode;
}
	
void CErrorInfoParser::SetMess(std::string& strMes)
{
	strErrMess = 	strMes;
}
	
void CErrorInfoParser::SetMess(const char *pMes)
{
	strErrMess = pMes;
}

int CErrorInfoParser::GetCode()
{
	return iErrCode;
}	

std::string CErrorInfoParser::GetMessage()
{
	return strErrMess;
}	

}//namespace base_tools