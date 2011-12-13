#include <tchar.h>
#include <windows.h>
#include "ex3_common.h"

/////////////////////////////////////////////////////////////////////////////
// note! we don't have a console so we don't print any error messages
// if anything goes wrong, there's no way to know about it...
// but these are the excercise requirements
/////////////////////////////////////////////////////////////////////////////

typedef struct {
	int delay;
	HANDLE ctrl_file;
	HANDLE log_file;
	HANDLE exit_evt;
	bool exit_evt_set;
	HANDLE file_mtx;
	bool file_mtx_owned;
	HANDLE can_write_sem;
	bool discard_last_write;
	HANDLE can_read_sem;
	DWORD log_sequence;
} program_state_t;


bool wait_for_object(program_state_t * state, HANDLE hobj)
{
	HANDLE waitees[] = {state->exit_evt, hobj};
	DWORD res = WaitForMultipleObjects(sizeof(waitees) / sizeof(waitees[0]), waitees, 
		FALSE, INFINITE);
	if (res == WAIT_OBJECT_0) {
		state->exit_evt_set = true;
		return false;
	}
	else if (res == WAIT_OBJECT_0 + 1) {
		return true;
	}
	else {
		return false;
	}
}

bool read_at(HANDLE hfile, DWORD pos, void * buf, DWORD count)
{
	DWORD actual;
	if (SetFilePointer(hfile, pos, 0, FILE_BEGIN) == INVALID_SET_FILE_POINTER) {
		return false;
	}
	if (!ReadFile(hfile, buf, count, &actual, NULL)) {
		return false;
	}
	if (actual != count) {
		// oops: actual read count != what we requested
		return false;
	}
	return true;
}

bool write_at(HANDLE hfile, DWORD pos, const void * buf, DWORD count)
{
	DWORD actual;
	if (SetFilePointer(hfile, pos, 0, FILE_BEGIN) == INVALID_SET_FILE_POINTER) {
		return false;
	}
	if (!WriteFile(hfile, buf, count, &actual, NULL)) {
		return false;
	}
	if (actual != count) {
		// oops: actual write count != what we requested
		return false;
	}
	return true;
}

bool init_program_state(program_state_t * state, _TCHAR * logfile, _TCHAR * ctrlfile, int delay_ms)
{
	state->delay = delay_ms;
	state->can_read_sem = NULL;
	state->can_write_sem = NULL;
	state->discard_last_write = false;
	state->exit_evt = NULL;
	state->exit_evt_set = false;
	state->file_mtx = NULL;
	state->file_mtx_owned = false;
	state->ctrl_file = INVALID_HANDLE_VALUE;
	state->log_file = INVALID_HANDLE_VALUE;
	state->log_sequence = 0;

	state->can_read_sem = CreateSemaphore(NULL, 0, 0, EX3_READ_SEM);
	if (state->can_read_sem == NULL) {
		return false;
	}
	if (GetLastError() != ERROR_ALREADY_EXISTS) {
		return false;
	}

	state->can_write_sem = CreateSemaphore(NULL, 0, 0, EX3_WRITE_SEM);
	if (state->can_write_sem == NULL) {
		return false;
	}
	if (GetLastError() != ERROR_ALREADY_EXISTS) {
		return false;
	}

	state->exit_evt = CreateEvent(NULL, TRUE, FALSE, EX3_EXIT_EVT);
	if (state->exit_evt == NULL) {
		return false;
	}
	if (GetLastError() != ERROR_ALREADY_EXISTS) {
		return false;
	}

	state->file_mtx = CreateMutex(NULL, FALSE, EX3_FILE_MTX);
	if (state->file_mtx == NULL) {
		return false;
	}
	if (GetLastError() != ERROR_ALREADY_EXISTS) {
		return false;
	}

	state->log_file = CreateFile(logfile, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL , NULL);
	if (state->log_file == INVALID_HANDLE_VALUE) {
		return false;
	}

	state->ctrl_file = CreateFile(ctrlfile, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL , NULL);
	if (state->ctrl_file == INVALID_HANDLE_VALUE) {
		return false;
	}

	return true;
}

void fini_program_state(program_state_t * state)
{
	if (state->can_read_sem != NULL) {
		CloseHandle(state->can_read_sem);
		state->can_read_sem = NULL;
	}
	if (state->can_write_sem != NULL) {
		if (state->discard_last_write) {
			// the last write needs to be discarded (otherwise we're 
			// taking a slot for nothing)
			ReleaseSemaphore(state->can_write_sem, 1, NULL);
			state->discard_last_write = false;
		}
		CloseHandle(state->can_write_sem);
		state->can_write_sem = NULL;
	}
	if (state->file_mtx != NULL) {
		if (state->file_mtx_owned) {
			ReleaseMutex(state->file_mtx);
			state->file_mtx_owned = false;
		}
		CloseHandle(state->file_mtx);
		state->file_mtx = NULL;
	}
	if (state->exit_evt != NULL) {
		CloseHandle(state->exit_evt);
		state->exit_evt = NULL;
	}
	if (state->ctrl_file != INVALID_HANDLE_VALUE) {
		CloseHandle(state->ctrl_file);
		state->ctrl_file = INVALID_HANDLE_VALUE;
	}
	if (state->log_file != INVALID_HANDLE_VALUE) {
		CloseHandle(state->log_file);
		state->log_file = INVALID_HANDLE_VALUE;
	}
}

bool write_log_record(program_state_t * state)
{
	DWORD slot;
	DWORD record[4];

	record[0] = GetCurrentProcessId();
	record[1] = GetTickCount();
	record[2] = state->log_sequence;
	record[3] = record[0] ^ record[1] ^ record[2];

	if (!read_at(state->ctrl_file, 0, &slot, sizeof(slot))) {
		return false;
	}
	if (!write_at(state->log_file, (slot % MAX_LOG_RECORDS) * sizeof(record), record, sizeof(record))) {
		return false;
	}
	slot++;
	if (!write_at(state->ctrl_file, 0, &slot, sizeof(slot))) {
		return false;
	}

	// advance sequence
	state->log_sequence++;
	return true;
}

bool producer_loop(program_state_t * state)
{
	while (true) {
		if (!wait_for_object(state, state->can_write_sem)) {
			break;
		}
		// we got the semaphore, meaning we have room in the log file
		// now let's take the file mutex
		state->discard_last_write = true;
		if (!wait_for_object(state, state->file_mtx)) {
			break;
		}
		state->file_mtx_owned = true;
		
		// read index from ctrl file, write log and update ctrl file
		if (!write_log_record(state)) {
			break;
		}
		
		// mark the write as successful
		state->discard_last_write = false;
		state->file_mtx_owned = false;
		ReleaseMutex(state->file_mtx);
		// tell reader it can read now and release mutex
		ReleaseSemaphore(state->can_read_sem, 1, NULL);

		// Sleep(state->delay);
		if (WaitForSingleObject(state->exit_evt, state->delay) == WAIT_OBJECT_0) {
			state->exit_evt_set = true;
			break;
		}
	}

	// success iff exit_evt is set
	return state->exit_evt_set;
}


int _tmain(int argc, _TCHAR* argv[])
{
	int res = -1;
	program_state_t state;

	if (argc != 4) {
		return -1;
	}

	int delay_ms = _tstoi(argv[3]);
	if (delay_ms <= 0) {
		return -1;
	}

	if (!init_program_state(&state, argv[1], argv[2], delay_ms)) {
		goto cleanup;
	}
	if (!producer_loop(&state)) {
		goto cleanup;
	}
	res = (int)state.log_sequence - 1;

cleanup:
	fini_program_state(&state);
	return res;
}

