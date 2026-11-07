#pragma once

#include <msxml6.h>
#include <wrl/client.h>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

/// Normalize dial punctuation while preserving non-numeric SIP identifiers exactly.
static string directoryNumber(const string& input) {
    string result;
    for (unsigned char ch : trim(input)) {
        if (ch >= '0' && ch <= '9') result += static_cast<char>(ch);
        else if (ch == '+' && result.empty()) result += '+';
        else if (ch != ' ' && ch != '-' && ch != '(' && ch != ')' && ch != '.') return trim(input);
    }
    return result;
}

/// Own an Automation string, including output strings returned by MSXML.
struct DirectoryBstr {
    /// Owned BSTR; never copied or logged.
    BSTR value = nullptr;
    /// Allocate a UTF-16 XML/query string.
    explicit DirectoryBstr(const wstring& text) : value(text.empty() ? nullptr : SysAllocStringLen(text.data(), static_cast<UINT>(text.size()))) {}
    /// Release the Automation allocation on all parser exit paths.
    ~DirectoryBstr() { SysFreeString(value); }
    DirectoryBstr(const DirectoryBstr&) = delete;
    DirectoryBstr& operator=(const DirectoryBstr&) = delete;
};

/// Parse the existing Fanvil contact/calllog schema; reject HTML, malformed XML and DTDs.
static bool parseDirectoryXml(const string& body, bool history, std::map<string, string>& names) {
    if (body.empty() || body.size() > 4 * 1024 * 1024) return false;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return false;
    /// Balance only the COM initialization owned by this invocation.
    struct ComScope {
        /// Whether CoInitializeEx incremented the current apartment count.
        bool owned;
        /// Release COM after every DOM reference has been destroyed.
        ~ComScope() { if (owned) CoUninitialize(); }
    } scope{SUCCEEDED(initialized)};
    Microsoft::WRL::ComPtr<IXMLDOMDocument2> document;
    if (FAILED(CoCreateInstance(__uuidof(DOMDocument60), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&document)))) return false;
    document->put_async(VARIANT_FALSE);
    document->put_validateOnParse(VARIANT_FALSE);
    document->put_resolveExternals(VARIANT_FALSE);
    DirectoryBstr prohibit(L"ProhibitDTD");
    VARIANT disabled{}; disabled.vt = VT_BOOL; disabled.boolVal = VARIANT_TRUE;
    if (FAILED(document->setProperty(prohibit.value, disabled))) return false;
    DirectoryBstr xml(utf8ToWide(body));
    VARIANT_BOOL loaded = VARIANT_FALSE;
    if (FAILED(document->loadXML(xml.value, &loaded)) || loaded != VARIANT_TRUE) return false;
    Microsoft::WRL::ComPtr<IXMLDOMElement> root;
    DirectoryBstr rootName(L"");
    if (FAILED(document->get_documentElement(&root)) || !root || FAILED(root->get_nodeName(&rootName.value))) return false;
    const wstring tag = rootName.value ? rootName.value : L"";
    if (tag == L"html" || tag == L"HTML") return false;
    DirectoryBstr query(history ? L"//record" : L"//contact");
    Microsoft::WRL::ComPtr<IXMLDOMNodeList> entries;
    if (FAILED(document->selectNodes(query.value, &entries)) || !entries) return false;
    long count = 0; entries->get_length(&count);
    if (count > 20000) return false;
    if (!count && tag != L"contacts" && tag != L"phonebook" && tag != L"calllog" && tag != L"records" && tag != L"PhoneBook" && tag != L"CallLog") return false;
    std::map<string, string> parsed;
    for (long index = 0; index < count; ++index) {
        Microsoft::WRL::ComPtr<IXMLDOMNode> entry;
        if (FAILED(entries->get_item(index, &entry)) || !entry) return false;
        const auto text = [&](const wchar_t* field) {
            DirectoryBstr path(field);
            Microsoft::WRL::ComPtr<IXMLDOMNode> node;
            DirectoryBstr value(L"");
            if (FAILED(entry->selectSingleNode(path.value, &node)) || !node || FAILED(node->get_text(&value.value))) return string();
            return trim(wideToUtf8(value.value ? value.value : L""));
        };
        const string name = text(L"name");
        if (name.empty() || name.size() > 1024 || name.find_first_of("\r\n") != string::npos) continue;
        for (const wchar_t* field : {L"tel1", L"tel2", L"tel3", L"num"}) {
            if (history != (wcscmp(field, L"num") == 0)) continue;
            const string number = directoryNumber(text(field));
            if (!number.empty() && number.size() <= 128 && number.find_first_of("\r\n\t") == string::npos && number != name) parsed.emplace(number, name);
        }
    }
    names.swap(parsed);
    return true;
}
