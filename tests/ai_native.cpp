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
    service.stop();restored.stop();puts("AI_NATIVE_OK profiles binding dual_sources context_containment preview stdio failure output_limit cancel no_automatic_resend");
}
