//
// Created by htsong on 2023/12/06.
//
#ifndef BASE_TOOLS_MTK_H_
#define BASE_TOOLS_MTK_H_

#include <string>


typedef struct{
    std::string imsi;
    std::string iccid;
} sSimInfo;

typedef struct{ 
    int cid;
    int pcid;
    int tac;
    std::string rat;
    int state;
} sNwRegInfo;

typedef struct{ 
    int rsrp;
    int rsrq;
    int sinr;
    int cqi;
    std::string level;
} sNwStrengthInfo;

namespace base_tools {
    namespace mtk {
        sSimInfo getSimInfo();
        std::string getImei();
        void getNwInfo(sNwRegInfo &nwRegInfo, sNwStrengthInfo &nwStrengthInfo);

    } // namespace mtk
} // namespace base_tools


#endif  // BASE_TOOLS_UTIL_H_