#include <windows.h>
#include <iostream>
#include <iterator>
#include <string>
#include <fstream>

/// Synthetic stdio executable; never loads credentials, models, project files or network APIs.
int main(int argc,char** argv) {
    if(argc>1 && std::string(argv[1])=="--child") {Sleep(300);std::cout<<"# Delayed child result";return 0;}
    if(argc>1 && std::string(argv[1])=="--version"){std::cout<<"fixture 1.0";return 0;}
    if(argc>1 && std::string(argv[1])=="--help"){std::cout<<"Currently supported: (synthetic-model, synthetic-model-2)";return 0;}
    if(argc>1 && (std::string(argv[1])=="--list-models" || std::string(argv[1])=="models")){std::cout<<"synthetic-model\nsynthetic-model-2\n";return 0;}
    if(argc>1 && std::string(argv[1])=="provider"){std::cout<<"{\"models\":{\"synthetic-model\":{}},\"providers\":{\"fake\":{\"api_key\":\"DO_NOT_PERSIST\"}}}";return 0;}
    if(argc>1 && std::string(argv[1])=="app-server") {
        std::string line;while(std::getline(std::cin,line)){if(line.find("initialize\"")!=std::string::npos)std::cout<<"{\"id\":1,\"result\":{}}\n"<<std::flush;else if(line.find("model/list")!=std::string::npos){std::cout<<"{\"id\":2,\"result\":{\"data\":[{\"id\":\"synthetic-model\",\"model\":\"synthetic-model\"}],\"nextCursor\":null}}\n"<<std::flush;}}
        return 0;
    }
    const std::string input((std::istreambuf_iterator<char>(std::cin)),std::istreambuf_iterator<char>());
        for(int i=1;i+1<argc;++i)if(std::string(argv[i])=="--output-last-message" || std::string(argv[i])=="-o"){std::ofstream out(argv[i+1],std::ios::binary);out<<"# Synthetic final result";}
    if(input.find("FIXTURE_LAUNCHER")!=std::string::npos) {
        wchar_t exe[32768]{};GetModuleFileNameW(NULL,exe,32768);std::wstring command=L"\""+std::wstring(exe)+L"\" --child";
        STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESTDHANDLES;startup.hStdInput=GetStdHandle(STD_INPUT_HANDLE);startup.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE);startup.hStdError=GetStdHandle(STD_ERROR_HANDLE);PROCESS_INFORMATION child{};
        if(!CreateProcessW(exe,&command[0],NULL,NULL,TRUE,CREATE_NO_WINDOW,NULL,NULL,&startup,&child))return 9;
        CloseHandle(child.hThread);CloseHandle(child.hProcess);return 0;
    }
    if(input.find("FIXTURE_HANG")!=std::string::npos) {Sleep(60000);return 0;}
    if(input.find("FIXTURE_FAILURE")!=std::string::npos) {std::cerr<<"synthetic failure";return 7;}
    if(input.find("FIXTURE_PROVIDER_ERROR")!=std::string::npos) {std::cout<<"400 synthetic provider error";return 0;}
    if(input.find("FIXTURE_REASONING_ERROR")!=std::string::npos) {std::cerr<<"400 the reasoning effort value is not supported by the current model";return 0;}
    if(input.find("FIXTURE_OVERSIZE")!=std::string::npos) {std::cout<<std::string(3*1024*1024,'x');return 0;}
    if(input.find("PHONERECORD_AI_OK")!=std::string::npos) {std::cout<<"PHONERECORD_AI_OK";return 0;}
    std::cout<<"# Synthetic analysis\n\nUTF-8: 使用者需求\nInput bytes: "<<input.size()<<"\n";
    return 0;
}
