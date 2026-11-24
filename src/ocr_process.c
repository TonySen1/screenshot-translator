#include "ocr_process.h"
#include "util.h"
#include "cJSON.h"

#include <objbase.h>
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>

#define OCR_START_MS 45000
#define OCR_REQUEST_MS 12000
#define OCR_MAX_REPLY (4u * 1024u * 1024u)
#define OCR_MAX_DIMENSION 8192
#define OCR_MAX_PIXELS 20000000u

typedef struct OcrProcess {
    wchar_t dir[MAX_PATH];
    HANDLE job;
    HANDLE pipe;
    HANDLE event;
    HANDLE process;
    ULONGLONG ready_deadline;
    BOOL ready;
} OcrProcess;

static OcrProcess g_ocr;

void ocr_stop(void)
{
    if (g_ocr.job) CloseHandle(g_ocr.job);
    if (g_ocr.pipe) CloseHandle(g_ocr.pipe);
    if (g_ocr.event) CloseHandle(g_ocr.event);
    if (g_ocr.process) CloseHandle(g_ocr.process);
    g_ocr.job = g_ocr.pipe = g_ocr.event = g_ocr.process = NULL;
    g_ocr.ready = FALSE;
}

static DWORD start_process(void)
{
    wchar_t python[MAX_PATH], worker[MAX_PATH], command[MAX_PATH * 2 + 32];
    if (!g_ocr.dir[0] ||
        swprintf_s(python, MAX_PATH, L"%ls\\python.exe", g_ocr.dir) < 0 ||
        swprintf_s(worker, MAX_PATH, L"%ls\\worker.py", g_ocr.dir) < 0) {
        return ERROR_PATH_NOT_FOUND;
    }
    if (GetFileAttributesW(python) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(worker) == INVALID_FILE_ATTRIBUTES) {
        return ERROR_FILE_NOT_FOUND;
    }
    swprintf_s(command, _countof(command), L"\"%ls\" -B -u \"%ls\"", python, worker);

    GUID id;
    wchar_t guid[40], pipe_name[96];
    if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(&id, guid, _countof(guid))) {
        return ERROR_GEN_FAILURE;
    }
    swprintf_s(pipe_name, _countof(pipe_name), L"\\\\.\\pipe\\ScreenshotTranslator.OCR.%ls", guid);

    HANDLE child_pipe = INVALID_HANDLE_VALUE;
    HANDLE child_output = NULL;
    HANDLE child_error = INVALID_HANDLE_VALUE;
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = NULL;
    BOOL attrs_ready = FALSE;
    PROCESS_INFORMATION pi = {0};
    DWORD error = ERROR_SUCCESS;
    SIZE_T attr_bytes = 0;
    SECURITY_ATTRIBUTES inherit = {sizeof(inherit), NULL, TRUE};
    STARTUPINFOEXW si = {0};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

    g_ocr.job = CreateJobObjectW(NULL, NULL);
    if (!g_ocr.job || !SetInformationJobObject(g_ocr.job, JobObjectExtendedLimitInformation,
                                              &limits, sizeof(limits))) goto failed;
    g_ocr.event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_ocr.event) goto failed;
    HANDLE pipe = CreateNamedPipeW(pipe_name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
                                             FILE_FLAG_FIRST_PIPE_INSTANCE,
                                    PIPE_TYPE_BYTE | PIPE_REJECT_REMOTE_CLIENTS,
                                    1, 65536, 65536, 0, NULL);
    if (pipe == INVALID_HANDLE_VALUE) goto failed;
    g_ocr.pipe = pipe;
    child_pipe = CreateFileW(pipe_name, GENERIC_READ | GENERIC_WRITE, 0, &inherit,
                             OPEN_EXISTING, 0, NULL);
    if (child_pipe == INVALID_HANDLE_VALUE) goto failed;
    if (!DuplicateHandle(GetCurrentProcess(), child_pipe, GetCurrentProcess(),
                         &child_output, 0, TRUE, DUPLICATE_SAME_ACCESS)) goto failed;
    child_error = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              &inherit, OPEN_EXISTING, 0, NULL);
    if (child_error == INVALID_HANDLE_VALUE) goto failed;

    InitializeProcThreadAttributeList(NULL, 1, 0, &attr_bytes);
    attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)heap_alloc(attr_bytes);
    if (!attrs) { SetLastError(ERROR_OUTOFMEMORY); goto failed; }
    if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attr_bytes)) goto failed;
    attrs_ready = TRUE;
    HANDLE std_handles[] = {child_pipe, child_output, child_error};
    if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                    std_handles, sizeof(std_handles), NULL, NULL)) goto failed;

    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = child_pipe;
    si.StartupInfo.hStdOutput = child_output;
    si.StartupInfo.hStdError = child_error;
    si.lpAttributeList = attrs;
    if (!CreateProcessW(python, command, NULL, NULL, TRUE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                        NULL, g_ocr.dir, &si.StartupInfo, &pi)) goto failed;
    if (!AssignProcessToJobObject(g_ocr.job, pi.hProcess) ||
        ResumeThread(pi.hThread) == (DWORD)-1) goto failed;
    g_ocr.process = pi.hProcess;
    g_ocr.ready_deadline = GetTickCount64() + OCR_START_MS;
    LOG_INFO("ocr", L"PaddleOCR 后台预热已启动");
    goto done;

failed:
    error = GetLastError();
    if (pi.hProcess) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
    }
    ocr_stop();
done:
    if (pi.hThread) CloseHandle(pi.hThread);
    if (attrs_ready) DeleteProcThreadAttributeList(attrs);
    heap_free(attrs);
    if (child_pipe != INVALID_HANDLE_VALUE) CloseHandle(child_pipe);
    if (child_output) CloseHandle(child_output);
    if (child_error != INVALID_HANDLE_VALUE) CloseHandle(child_error);
    return error;
}

void ocr_preload(const wchar_t *exe_dir)
{
    if (!exe_dir || swprintf_s(g_ocr.dir, MAX_PATH, L"%ls\\ocr", exe_dir) < 0) {
        g_ocr.dir[0] = 0;
        return;
    }
    DWORD error = start_process();
    if (error) LOG_WARN("ocr", L"未预热 PaddleOCR（错误 %lu）", error);
}

static BOOL transfer(void *data, DWORD size, BOOL write, ULONGLONG deadline)
{
    unsigned char *cursor = (unsigned char *)data;
    while (size != 0) {
        OVERLAPPED op = {0};
        op.hEvent = g_ocr.event;
        ResetEvent(op.hEvent);
        DWORD transferred = 0;
        BOOL ok = write ? WriteFile(g_ocr.pipe, cursor, size, &transferred, &op)
                        : ReadFile(g_ocr.pipe, cursor, size, &transferred, &op);
        if (!ok) {
            if (GetLastError() != ERROR_IO_PENDING) return FALSE;
            ULONGLONG now = GetTickCount64();
            DWORD remaining = now < deadline ? (DWORD)(deadline - now) : 0;
            DWORD wait = WaitForSingleObject(op.hEvent, remaining);
            if (wait != WAIT_OBJECT_0) {
                CancelIoEx(g_ocr.pipe, &op);
                GetOverlappedResult(g_ocr.pipe, &op, &transferred, TRUE);
                SetLastError(wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : ERROR_GEN_FAILURE);
                return FALSE;
            }
            if (!GetOverlappedResult(g_ocr.pipe, &op, &transferred, FALSE)) return FALSE;
        }
        if (!transferred) { SetLastError(ERROR_BROKEN_PIPE); return FALSE; }
        cursor += transferred;
        size -= transferred;
    }
    return TRUE;
}

static cJSON *read_frame(ULONGLONG deadline)
{
    uint32_t length = 0;
    if (!transfer(&length, sizeof(length), FALSE, deadline)) return NULL;
    if (!length || length > OCR_MAX_REPLY) { SetLastError(ERROR_INVALID_DATA); return NULL; }
    char *data = (char *)heap_alloc((size_t)length + 1);
    if (!data) { SetLastError(ERROR_OUTOFMEMORY); return NULL; }
    cJSON *json = NULL;
    DWORD error = ERROR_SUCCESS;
    if (transfer(data, length, FALSE, deadline)) {
        data[length] = 0;
        json = cJSON_ParseWithLengthOpts(data, (size_t)length + 1, NULL, TRUE);
        if (!cJSON_IsObject(json)) { cJSON_Delete(json); json = NULL; error = ERROR_INVALID_DATA; }
    } else {
        error = GetLastError();
    }
    heap_free(data);
    if (!json) SetLastError(error);
    return json;
}

static BOOL set_error(OcrResult *out, const wchar_t *message)
{
    size_t size = wcslen(message) + 1;
    out->error = (wchar_t *)heap_alloc(size * sizeof(wchar_t));
    if (out->error) wstr_copy(out->error, size, message);
    return FALSE;
}

static BOOL process_failure(OcrResult *out, DWORD error, BOOL starting)
{
    LOG_WARN("ocr", L"PaddleOCR %ls失败（错误 %lu）", starting ? L"启动" : L"识别", error);
    ocr_stop();
    if (error == ERROR_TIMEOUT) {
        return set_error(out, starting ? L"PaddleOCR 加载超时，请重试。"
                                      : L"本地识别超时，请缩小选区后重试。");
    }
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
        return set_error(out, L"未找到 PaddleOCR。请完整解压发布包，或运行 setup-ocr.ps1。\n也可从托盘选择「看图翻译（上传截图）」。");
    }
    return set_error(out, L"PaddleOCR 运行失败，请重试或检查本地 OCR 文件。截图未上传。");
}

BOOL ocr_recognize(const ImageBuffer *image, OcrResult *out)
{
    *out = (OcrResult){0};
    if (!image || !image->bits || image->width <= 0 || image->height <= 0 ||
        image->width > OCR_MAX_DIMENSION || image->height > OCR_MAX_DIMENSION ||
        (size_t)image->width * (size_t)image->height > OCR_MAX_PIXELS ||
        image->stride != image->width * 4) {
        return set_error(out, L"选区过大或无效，请缩小选区后重试。");
    }
    if (!g_ocr.process || (g_ocr.ready && WaitForSingleObject(g_ocr.process, 0) == WAIT_OBJECT_0)) {
        ocr_stop();
        DWORD error = start_process();
        if (error) return process_failure(out, error, TRUE);
    }
    if (!g_ocr.ready) {
        cJSON *init = read_frame(g_ocr.ready_deadline);
        if (!init) return process_failure(out, GetLastError(), TRUE);
        g_ocr.ready = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(init, "ready"));
        if (!g_ocr.ready) {
            const char *message = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(init, "error"));
            if (message) out->error = u8_to_u16(message);
        }
        cJSON_Delete(init);
        if (!g_ocr.ready) {
            if (!out->error) return process_failure(out, ERROR_INVALID_DATA, TRUE);
            ocr_stop();
            return FALSE;
        }
    }

    ULONGLONG start = GetTickCount64();
    ULONGLONG deadline = start + OCR_REQUEST_MS;
    uint32_t dimensions[2] = {(uint32_t)image->width, (uint32_t)image->height};
    DWORD bytes = dimensions[0] * dimensions[1] * 4u;
    if (!transfer(dimensions, sizeof(dimensions), TRUE, deadline) ||
        !transfer(image->bits, bytes, TRUE, deadline)) {
        return process_failure(out, GetLastError(), FALSE);
    }
    cJSON *reply = read_frame(deadline);
    if (!reply) return process_failure(out, GetLastError(), FALSE);
    const char *message = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(reply, "error"));
    const char *text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(reply, "text"));
    if (message) out->error = u8_to_u16(message);
    else if (text) {
        out->text = u8_to_u16(text);
        out->lines = cJSON_DetachItemFromObjectCaseSensitive(reply, "lines");
        out->table = cJSON_DetachItemFromObjectCaseSensitive(reply, "table");
    }
    size_t line_count = (size_t)cJSON_GetArraySize(out->lines);
    cJSON_Delete(reply);
    if (out->error) return FALSE;
    if (!out->text) return set_error(out, L"无法读取 PaddleOCR 结果，请重试。");
    LOG_INFO("ocr", L"本地 OCR 完成：%zu 行，%llu ms", line_count, GetTickCount64() - start);
    return TRUE;
}

void ocr_result_free(OcrResult *out)
{
    heap_free(out->text);
    heap_free(out->error);
    cJSON_Delete(out->lines);
    cJSON_Delete(out->table);
    *out = (OcrResult){0};
}
