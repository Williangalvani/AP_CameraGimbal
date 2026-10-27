#pragma once
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Linux thermal zones contain millidegrees; SigmaStar TEMP_R contains the
// labelled integer Celsius value "Temperature N". Never infer units by size.
inline bool apcam_parse_soc_temperature(const char *text,bool labelled_celsius,int *millidegrees)
{
    if(!text || !millidegrees)return false;
    if(labelled_celsius) {
        const char prefix[]="Temperature ";
        if(std::strncmp(text,prefix,sizeof(prefix)-1))return false;
        text+=sizeof(prefix)-1;
    }
    errno=0;char *end=nullptr;const long value=std::strtol(text,&end,10);
    if(errno==ERANGE || end==text)return false;
    while(*end && std::isspace(static_cast<unsigned char>(*end)))++end;
    const long lower=labelled_celsius?-40:-40000,upper=labelled_celsius?150:150000;
    if(*end || value<lower || value>upper)return false;
    *millidegrees=int(value)*(labelled_celsius?1000:1);
    return true;
}
inline bool apcam_read_soc_temperature(const char *path,bool labelled_celsius,int *millidegrees)
{
    FILE *file=std::fopen(path,"r");if(!file)return false;
    char text[64];
    const bool complete=std::fgets(text,sizeof(text),file) && std::fgetc(file)==EOF && !std::ferror(file);
    std::fclose(file);
    return complete && apcam_parse_soc_temperature(text,labelled_celsius,millidegrees);
}
// HiSilicon /proc/umap/pm is a table; the value follows "cur_temp:" in Celsius.
inline bool apcam_read_soc_temperature_field(const char *path,const char *field,int *millidegrees)
{
    FILE *file=std::fopen(path,"r");if(!file)return false;
    char line[256];bool ok=false;
    const size_t field_length=std::strlen(field);
    while(!ok && std::fgets(line,sizeof(line),file)) {
        const char *at=std::strstr(line,field);
        if(!at)continue;
        at+=field_length;
        while(*at==' ' || *at=='\t')++at;
        errno=0;char *end=nullptr;const long value=std::strtol(at,&end,10);
        if(errno==ERANGE || end==at || (*end && !std::isspace(static_cast<unsigned char>(*end))))break;
        if(value<-40 || value>150)break;
        *millidegrees=int(value)*1000;ok=true;
    }
    std::fclose(file);
    return ok;
}
