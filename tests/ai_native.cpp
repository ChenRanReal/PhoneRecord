#include "../cpp/phonerecord.cpp"
#include <cassert>

/// Wait for actual process completion with a bounded test deadline, not a guessed sleep.
static AiState completed(AiAnalysis& service) {
    const ULONGLONG start=GetTickCount64();
    while(service.state().busy && GetTickCount64()-start<15000) Sleep(10);
    assert(!service.state().busy);return service.state();
}

/// Validate production profile storage, source assembly, project containment and owned stdio.
int wmain(int argc,wchar_t** argv) {
    assert(argc>=2);
    namespace fs=std::filesystem;
    const fs::path root=fs::current_path()/L"AI 測試";fs::create_directories(root/L"recordings");fs::create_directories(root/L"project");
    const wstring wav=(root/L"recordings"/L"call.wav").native(), config=(root/L"config.ini").native();
    g_cfg.recordDir=wideToUtf8((root/L"recordings").native());
    assert(SpeechService::write(wav,"synthetic WAV identity"));
    const string sense="{\"status\":\"complete\",\"text\":\"需要修改排配\",\"segments\":[{\"start\":1,\"end\":3,\"text\":\"需要修改排配\"}]}";
    const string qwen="{\"status\":\"complete\",\"text\":\"不需要修改排配\",\"segments\":[{\"start\":1,\"end\":4,\"text\":\"不需要修改排配\"}]}";
    assert(SpeechService::write(wav+L".transcript.json",sense));assert(SpeechService::write(wav+L".qwen3asr.transcript.json",qwen));
    assert(SpeechService::write((root/L"project"/L"README.md").native(),"# 測試專案\nExisting assignment flow."));
    AiAnalysis service;service.start(config);
    AiProfile cli;cli.id="cli-1";cli.kind="cli";cli.name="本地測試";cli.type="stdin";cli.path=wideToUtf8(fs::absolute(argv[1]).native());cli.timeout=10;
    cli.models="synthetic-model\nsynthetic-model-2";assert(service.save(cli));
    AiProfile prompt;prompt.id="prompt-1";prompt.kind="prompt";prompt.name="需求";prompt.body="Separate confirmed requirements and ASR conflicts.";assert(service.save(prompt));
    AiProfile project;project.id="project-1";project.kind="project";project.name="測試專案";project.path=wideToUtf8((root/L"project").native());project.cli=cli.id;project.prompt=prompt.id;project.references="README.md";assert(service.save(project));
    assert(!service.erase(cli.id));assert(!service.erase(prompt.id));assert(service.bind(wav,project.id));assert(service.binding(wav)==project.id);
    AiAnalysis restored;restored.start(config);assert(restored.profiles().size()==3);assert(restored.find("project-1")->cli=="cli-1");assert(restored.find("cli-1")->models==cli.models);
    for(const auto& tool:kAiCliTools){AiProfile provider=cli;provider.id=tool.id;provider.type=tool.id;assert(service.save(provider));}
    AiRequest request;request.wav=wav;request.cli=cli;request.project=project;request.prompt=prompt;
    string input,error;assert(AiAnalysis::assemble(request,input,error));assert(input.find("需要修改排配")!=string::npos && input.find("不需要修改排配")!=string::npos);assert(input.find("README.md")!=string::npos);assert(input.find("SHA256=")!=string::npos);
    assert(AiAnalysis::digest("abc")=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    request.project.references="../outside.md";input.clear();assert(!AiAnalysis::assemble(request,input,error) && error=="reference_invalid");request.project=project;
    request.project.references="\\outside.md";input.clear();assert(!AiAnalysis::assemble(request,input,error) && error=="reference_invalid");request.project=project;
    request.project.references="appsettings.json";input.clear();assert(!AiAnalysis::assemble(request,input,error) && error=="reference_invalid");request.project=project;
    request.project.references="missing.md";input.clear();assert(!AiAnalysis::assemble(request,input,error) && error=="reference_missing");request.project=project;
    assert(SpeechService::write(wav+L".qwen3asr.transcript.json","{\"status\":\"running\",\"text\":\"partial\"}"));input.clear();assert(!AiAnalysis::assemble(request,input,error) && error=="source_incomplete");
    request.qwen=false;assert(AiAnalysis::assemble(request,input,error));request.qwen=true;assert(SpeechService::write(wav+L".qwen3asr.transcript.json",qwen));
    assert(SpeechService::write(wav+L".transcript.json","{\"status\":\"complete\",\"text\":\"stale\",\"sourceSha256\":\"wrong\"}"));assert(!AiAnalysis::assemble(request,input,error) && error=="source_changed");assert(SpeechService::write(wav+L".transcript.json",sense));
    const string before=SpeechService::read(wav);assert(service.run(request,true));auto state=completed(service);assert(state.step=="preview" && fs::exists(state.result));
    assert(service.run(request));state=completed(service);assert(state.step=="complete");const string result=SpeechService::read(state.result);assert(result.find("使用者需求")!=string::npos);
    const string latest=SpeechService::read(wav+L".ai-latest.json");assert(!latest.empty());
    request.extra="FIXTURE_LAUNCHER";assert(service.run(request));state=completed(service);assert(state.step=="complete");assert(SpeechService::read(state.result).find("Delayed child result")!=string::npos);
    assert(SpeechService::write(wav+L".ai-latest.json",latest));
    request.extra="FIXTURE_FAILURE";assert(service.run(request));state=completed(service);assert(state.step=="process_failed");assert(SpeechService::read(wav+L".ai-latest.json")==latest);
    request.extra="FIXTURE_PROVIDER_ERROR";assert(service.run(request));assert(completed(service).step=="provider_error");assert(SpeechService::read(wav+L".ai-latest.json")==latest);
    request.extra="FIXTURE_REASONING_ERROR";assert(service.run(request));assert(completed(service).step=="reasoning_unsupported");assert(SpeechService::read(wav+L".ai-latest.json")==latest);
    request.extra="FIXTURE_OVERSIZE";assert(service.run(request));assert(completed(service).step=="output_limit");
    request.extra="FIXTURE_HANG";assert(service.run(request));assert(!service.run(request));
    const ULONGLONG start=GetTickCount64();while(service.state().step!="running" && GetTickCount64()-start<3000)Sleep(10);service.cancel();assert(completed(service).step=="cancelled");
    assert(service.run(request));assert(completed(service).step=="timeout");
    request.extra="";request.test=true;assert(service.run(request));assert(completed(service).step=="complete");request.test=false;
    request.cli.path="missing-cli.exe";assert(service.run(request));assert(completed(service).step=="cli_missing");request.cli=cli;
    {std::lock_guard<std::mutex> guard(g_updateAdmission);g_updateInstalling=true;}assert(!service.run(request));{std::lock_guard<std::mutex> guard(g_updateAdmission);g_updateInstalling=false;}
    assert(SpeechService::read(wav)==before);assert(SpeechService::read(wav+L".transcript.json")==sense);
    if(argc>=3) {
        AiRequest real;real.test=true;real.cli=cli;real.cli.type="codebuddy";real.cli.path=wideToUtf8(argv[2]);real.cli.timeout=60;real.cli.effort="medium";
        assert(service.run(real));
        const ULONGLONG started=GetTickCount64();while(service.state().busy && GetTickCount64()-started<70000)Sleep(30);
        const auto observed=service.state();printf("CODEBUDDY_SYNTHETIC status=%s exit=%s\n",observed.step.c_str(),observed.code.c_str());
    }
    string endpoint, endpointError;
    assert(aiApiEndpoint("openai-chat","https://api.openai.com/v1",endpoint,endpointError) && endpoint=="https://api.openai.com/v1/chat/completions");
    assert(aiApiEndpoint("openai-chat","https://api.openai.com/v1/",endpoint,endpointError) && endpoint=="https://api.openai.com/v1/chat/completions");
    assert(aiApiEndpoint("openai-chat","https://api.openai.com",endpoint,endpointError) && endpoint=="https://api.openai.com/v1/chat/completions");
    assert(aiApiEndpoint("openai-chat","https://api.groq.com/openai/v1",endpoint,endpointError) && endpoint=="https://api.groq.com/openai/v1/chat/completions");
    assert(aiApiEndpoint("openai-chat","https://api.openai.com/v1/chat/completions",endpoint,endpointError) && endpoint=="https://api.openai.com/v1/chat/completions");
    assert(aiApiEndpoint("openai-responses","https://api.openai.com/v1",endpoint,endpointError) && endpoint=="https://api.openai.com/v1/responses");
    assert(aiApiEndpoint("anthropic","https://api.anthropic.com",endpoint,endpointError) && endpoint=="https://api.anthropic.com/v1/messages");
    assert(aiApiEndpoint("anthropic","https://api.anthropic.com/v1",endpoint,endpointError) && endpoint=="https://api.anthropic.com/v1/messages");
    assert(aiApiEndpoint("anthropic","https://api.minimaxi.com/anthropic",endpoint,endpointError) && endpoint=="https://api.minimaxi.com/anthropic/v1/messages");
    assert(aiApiEndpoint("openai-chat","https://example.openai.azure.com/openai/deployments/gpt/chat/completions?api-version=2024-10-21",endpoint,endpointError) && endpoint.find("api-version=2024-10-21")!=string::npos && aiApiEnds(endpoint.substr(0,endpoint.find('?')),"/chat/completions"));
    assert(aiApiEndpoint("openai-chat","http://127.0.0.1:11434/v1",endpoint,endpointError) && endpoint=="http://127.0.0.1:11434/v1/chat/completions");
    assert(aiApiEndpoint("openai-chat","http://localhost:11434/v1",endpoint,endpointError) && endpoint=="http://localhost:11434/v1/chat/completions");
    assert(aiApiEndpoint("openai-chat","http://[::1]:11434/v1",endpoint,endpointError) && endpoint=="http://[::1]:11434/v1/chat/completions");
    assert(!aiApiEndpoint("openai-chat","http://example.com/v1",endpoint,endpointError));
    assert(!aiApiEndpoint("openai-chat","https://user:pass@api.openai.com/v1",endpoint,endpointError));
    assert(!aiApiEndpoint("openai-chat","https://api.openai.com/v1?api_key=secret",endpoint,endpointError));
    const string chatBody=aiApiRequestBody("openai-chat","unit-model","hello");
    assert(chatBody.find("\"messages\"")!=string::npos && chatBody.find("unit-model")!=string::npos);
    assert(aiApiRequestBody("openai-responses","unit-model","hello").find("\"input\"")!=string::npos);
    assert(aiApiRequestBody("anthropic","unit-model","hello").find("\"max_tokens\":8192")!=string::npos);
    string extracted;
    assert(aiApiExtract("openai-chat","{\"choices\":[{\"message\":{\"content\":\"line\\nPHONERECORD_AI_OK\"}}]}",extracted) && extracted=="line\nPHONERECORD_AI_OK");
    assert(aiApiExtract("openai-chat","{\"choices\":[{\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"PHONERECORD_AI_OK\"}]}}]}",extracted) && extracted=="PHONERECORD_AI_OK");
    assert(aiApiExtract("openai-responses","{\"output\":[{\"type\":\"message\",\"content\":[{\"type\":\"output_text\",\"text\":\"PHONERECORD_AI_OK\"}]}]}",extracted) && extracted=="PHONERECORD_AI_OK");
    assert(aiApiExtract("anthropic","{\"content\":[{\"type\":\"thinking\",\"thinking\":\"thinking-secret\"},{\"type\":\"text\",\"text\":\"PHONERECORD_AI_OK\"}]}",extracted) && extracted=="PHONERECORD_AI_OK");
    const string apiKey="sk-test-Phonerecord-Key-9f3a";
    AiProfile leaked; leaked.secret=apiKey; leaked.name="hidden"; assert(AiAnalysis::encode(leaked).find(apiKey)==string::npos);
    const size_t beforeApi=service.profiles().size();
    AiProfile rejected; rejected.kind="api"; rejected.name="拒絕"; rejected.type="openai-chat"; rejected.model="unit-model"; rejected.timeout=30; rejected.path="http://example.com/v1";
    assert(!service.save(rejected)); rejected.path="https://user:pass@api.openai.com/v1"; assert(!service.save(rejected));
    rejected.path="https://api.openai.com/v1?api_key=secret"; assert(!service.save(rejected)); rejected.model="bad model"; rejected.path="https://api.openai.com/v1"; assert(!service.save(rejected));
    assert(service.profiles().size()==beforeApi);
    WSADATA sockets{}; assert(WSAStartup(MAKEWORD(2,2),&sockets)==0);
    SOCKET listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP); assert(listener!=INVALID_SOCKET);
    sockaddr_in address{}; address.sin_family=AF_INET; address.sin_addr.s_addr=htonl(INADDR_LOOPBACK); address.sin_port=0;
    assert(bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0 && listen(listener,4)==0);
    sockaddr_in bound{}; int boundLength=sizeof(bound); assert(getsockname(listener,reinterpret_cast<sockaddr*>(&bound),&boundLength)==0);
    const string port=std::to_string(ntohs(bound.sin_port));
    std::atomic<bool> stopServer{false}; std::mutex serverLock; std::vector<string> seenHeaders, seenBodies;
    std::thread server([&]{
        while(!stopServer.load()) {
            fd_set ready; FD_ZERO(&ready); FD_SET(listener,&ready); timeval wait{0,200000};
            if(select(0,&ready,NULL,NULL,&wait)<=0) continue;
            SOCKET client=accept(listener,NULL,NULL); if(client==INVALID_SOCKET) continue;
            string request; char buffer[4096];
            while(request.find("\r\n\r\n")==string::npos && request.size()<1024*1024) {const int count=recv(client,buffer,sizeof(buffer),0); if(count<=0) break; request.append(buffer,count);}
            size_t length=0; const size_t marker=request.find("Content-Length:");
            if(marker!=string::npos) length=strtoul(request.c_str()+marker+15,NULL,10);
            const size_t headerEnd=request.find("\r\n\r\n");
            if(headerEnd!=string::npos) while(request.size()<headerEnd+4+length && request.size()<2*1024*1024) {const int count=recv(client,buffer,sizeof(buffer),0); if(count<=0) break; request.append(buffer,count);}
            const size_t split=request.find("\r\n\r\n");
            string header=split==string::npos?request:request.substr(0,split), body=split==string::npos?"":request.substr(split+4);
            {std::lock_guard<std::mutex> lock(serverLock); seenHeaders.push_back(header); seenBodies.push_back(body);}
            string lower=header; for(char& ch:lower) if(ch>='A'&&ch<='Z') ch=static_cast<char>(ch+32);
            string payload="{\"error\":{\"message\":\"missing\"}}"; int status=404;
            if(lower.find("get ")==0 && lower.find("/models ")!=string::npos) {status=200; payload="{\"data\":[{\"id\":\"unit-a\"},{\"id\":\"unit-b\"}]}";}
            else if(lower.find("reject-me-key-value")!=string::npos) {status=401; payload="{\"error\":{\"message\":\"rejected reject-me-key-value\"}}";}
            else if(lower.find("/chat/completions ")!=string::npos) {status=200; payload="{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"PHONERECORD_AI_OK\"}}]}";}
            else if(lower.find("/responses ")!=string::npos) {status=200; payload="{\"output\":[{\"type\":\"message\",\"content\":[{\"type\":\"output_text\",\"text\":\"PHONERECORD_AI_OK\"}]}]}";}
            else if(lower.find("/messages ")!=string::npos) {status=200; payload="{\"content\":[{\"type\":\"thinking\",\"thinking\":\"thinking-secret\"},{\"type\":\"text\",\"text\":\"PHONERECORD_AI_OK\"}]}";}
            const string statusText=status==200?"OK":status==401?"Unauthorized":"Not Found";
            const string response="HTTP/1.1 "+std::to_string(status)+" "+statusText+"\r\nContent-Type: application/json\r\nContent-Length: "+std::to_string(payload.size())+"\r\nConnection: close\r\n\r\n"+payload;
            send(client,response.data(),static_cast<int>(response.size()),0); closesocket(client);
        }
    });
    AiProfile api; api.kind="api"; api.name="本地接口"; api.type="openai-chat"; api.path="http://127.0.0.1:"+port+"/v1"; api.model="unit-model"; api.timeout=10; api.secret=apiKey;
    assert(service.save(api));
    string apiId; for(const auto& profile:service.profiles()) if(profile.kind=="api" && profile.name=="本地接口") apiId=profile.id;
    assert(!apiId.empty());
    const string secretFile=SpeechService::read(config+L".ai-profiles.secrets",1024*1024);
    assert(secretFile.find("dpapi:")!=string::npos && secretFile.find(apiKey)==string::npos);
    AiAnalysis apiService; apiService.start(config);
    AiRequest call; call.test=true; call.cli=*apiService.find(apiId); assert(call.cli.secret.empty() && apiService.hasSecret(apiId));
    assert(apiService.run(call)); assert(completed(apiService).step=="complete");
    {std::lock_guard<std::mutex> lock(serverLock); bool authed=false, bodyOk=false; for(size_t i=0;i<seenHeaders.size();++i){if(seenHeaders[i].find("Bearer "+apiKey)!=string::npos) authed=true; if(seenBodies[i].find(apiKey)==string::npos && seenBodies[i].find("unit-model")!=string::npos && seenBodies[i].find("Connection test")!=string::npos) bodyOk=true;} assert(authed && bodyOk);}
    call.cli.type="openai-responses"; assert(apiService.run(call)); assert(completed(apiService).step=="complete");
    {std::lock_guard<std::mutex> lock(serverLock); bool input=false; for(const string& body:seenBodies) if(body.find("\"input\"")!=string::npos) input=true; assert(input);}
    call.cli.type="anthropic"; call.cli.path="http://127.0.0.1:"+port; assert(apiService.run(call)); auto anthropic=completed(apiService); assert(anthropic.step=="complete");
    assert(SpeechService::read(anthropic.result).find("thinking-secret")==string::npos);
    {std::lock_guard<std::mutex> lock(serverLock); bool anthropic=false; for(const string& header:seenHeaders) if(header.find("x-api-key: "+apiKey)!=string::npos && header.find("anthropic-version: 2023-06-01")!=string::npos) anthropic=true; assert(anthropic);}
    AiProfile kept=*apiService.find(apiId); kept.secret.clear(); assert(apiService.save(kept));
    call.cli=*apiService.find(apiId); call.cli.type="openai-chat"; call.cli.path="http://127.0.0.1:"+port+"/v1"; call.cli.secret.clear();
    assert(apiService.run(call)); assert(completed(apiService).step=="complete");
    assert(apiService.refreshApi(*apiService.find(apiId))); assert(completed(apiService).step=="models_ready");
    const string models=apiService.observation(apiId).models; assert(models.find("unit-a")!=string::npos && models.find("unit-b")!=string::npos);
    call.cli.secret="reject-me-key-value"; assert(apiService.run(call)); assert(completed(apiService).step=="provider_error");
    stopServer=true; server.join(); closesocket(listener);
    std::error_code scanError; bool leakedOnDisk=false;
    for(auto entry=fs::recursive_directory_iterator(root,scanError); entry!=fs::recursive_directory_iterator(); entry.increment(scanError)) {
        if(scanError || !entry->is_regular_file()) {scanError.clear(); continue;}
        const string bytes=SpeechService::read(entry->path().native(),8*1024*1024);
        if(bytes.find(apiKey)!=string::npos || bytes.find("reject-me-key-value")!=string::npos) leakedOnDisk=true;
    }
    assert(!leakedOnDisk);
    apiService.stop();
    service.stop();restored.stop();puts("AI_NATIVE_OK profiles binding dual_sources context_containment preview stdio failure output_limit cancel no_automatic_resend api_endpoint api_secret api_http");
}
