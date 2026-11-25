#pragma once

namespace Nilesoft
{
	namespace Diagnostics
	{
		using namespace Text;
	/*
	verb
	"open"			- Opens a file or a application
	"openas"		- Opens dialog when no program is associated to the extension
	"opennew"		- see MSDN
	"runas"			- In Windows 7 and Vista, opens the UAC dialog and in others, open the Run as... Dialog
	"null"			- Specifies that the operation is the default for the selected file type.
	"edit"			- Opens the default text editor for the file.
	"explore"		- Opens the Windows Explorer in the folder specified in lpDirectory.
	"properties"	- Opens the properties window of the file.
	"copy"			- see MSDN
	"cut"			- see MSDN
	"paste"			- see MSDN
	"pastelink"		- pastes a shortcut
	"delete"		- see MSDN
	"print"			- Start printing the file with the default application.
	"printto"		- see MSDN
	"find"			- Start a search
	*/
		class ShellExec
		{
			static constexpr auto m_open_as = L"openas";

		public:

			static string getpath(const wchar_t *file)
			{
				/*
					SearchPath reports the buffer it *needs* when the one it was
					given is too small, and leaves that buffer untouched:

						"If the return value is greater than nBufferLength, the
						return value is the size of the buffer required to hold
						the path."

					https://learn.microsoft.com/en-us/windows/win32/api/processenv/nf-processenv-searchpathw

					Testing only for zero therefore treated a too-long result as a
					hit and launched an empty path. Retry once at the size asked
					for instead.
				*/
				auto search = [](const wchar_t *name, const wchar_t *ext, string &out) -> uint32_t
				{
					out = IO::Contracts::fill_then_resize(
						[&](wchar_t *buffer, DWORD capacity)
						{
							return ::SearchPathW(nullptr, name, ext, capacity, buffer, nullptr);
						});
					return static_cast<uint32_t>(out.length());
				};

				string found;
				auto len = search(file, nullptr, found);
				if(len == 0)
				{
					if(!string::FindLast(file, L'.', false))
					{
						len = search(file, L".exe", found);
						if(len == 0)
							len = search(file, L".cmd", found);
						if(len == 0)
							len = search(file, L".bat", found);
					}
				}
				return len == 0 ? string(file) : found.move();
			}

			static uint32_t Run(const wchar_t* file, const wchar_t* parameters,
							 const wchar_t* directory, const wchar_t* verb, 
							 bool wait = false, int showCmd = SW_SHOWNORMAL)
			{
				bool openas = false;

				string f = getpath(file).move();

				if(f.is_null_whitespace())
					return FALSE;

				SHELLEXECUTEINFOW sei = { };
				sei.cbSize = sizeof(SHELLEXECUTEINFOW);

				/*
					SEE_MASK_NOASYNC is not optional here. It "must be specified if the
					thread calling ShellExecuteEx does not have a message loop or if the
					thread or process will terminate soon after ShellExecuteEx returns" -
					and the menu invokes commands from a detached worker that does exactly
					that. Without it the thread is gone before the DDE conversation can
					finish, and "failure to complete the conversation can result in an
					unsuccessful launch of the document": the item appears to do nothing,
					intermittently, only for file types launched through DDE.

					SEE_MASK_FLAG_LOG_USAGE is what the same page asks for "calls to this
					API that result from user interaction", which every menu command is.
					It is what keeps most-frequently-used lists and jump lists honest.

					  https://learn.microsoft.com/en-us/windows/win32/api/shellapi/ns-shellapi-shellexecuteinfow

					The flags accumulate. They used to be assigned over each other, so
					only the last assignment survived.
				*/
				sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI | SEE_MASK_FLAG_LOG_USAGE;

				if(!string::IsNullOrWhiteSpace(verb))
				{
					sei.lpVerb = verb;
					if(string::Equals(verb, m_open_as))
					{
						// The picker is driven through the item's own context menu.
						sei.fMask |= SEE_MASK_INVOKEIDLIST;
						openas = true;
					}
				}

				// Only asked for when there is something to wait on: with
				// SEE_MASK_INVOKEIDLIST the documentation says no process handle
				// comes back anyway.
				if(!openas)
					sei.fMask |= SEE_MASK_NOCLOSEPROCESS;

				sei.lpFile = f;
				if(!string::IsNullOrWhiteSpace(parameters))
					sei.lpParameters = parameters;

				if(!string::IsNullOrWhiteSpace(directory))
					sei.lpDirectory = directory;

				sei.nShow = showCmd;

				//Open the file using its associated program.
				auto ret = ::ShellExecuteExW(&sei);
				if(ret == FALSE)
				{
					auto dwError = ::GetLastError();
					if(dwError == ERROR_CANCELLED && string::Equals(verb, L"runas"))
					{
						// The operation was canceled by the user.
						// The user refused to allow privileges elevation.
						Logger::Debug(L"User did not allow elevation '%s'.", file);
					}
					//if(sei.fMask & SEE_MASK_NOCLOSEPROCESS)
					// The function failed, so report the error.
					// Err.LastDllError could also be used instead, if you wish.
					else switch(uintptr_t(sei.hInstApp))
					{
						//case ERROR_FILE_NOT_FOUND:
						case SE_ERR_FNF:
							Logger::Debug(L"Command File not found '%s'.", file);
							break;
							//case ERROR_PATH_NOT_FOUND:
						case SE_ERR_PNF:
							Logger::Debug(L"Command Path not found '%s'.", file);
							break;
						case SE_ERR_ACCESSDENIED:
							Logger::Debug(L"Command Access denied.");
							break;
						case SE_ERR_OOM:
							Logger::Debug(L"Command Out of memory.");
							break;
						case SE_ERR_DLLNOTFOUND:
							Logger::Debug(L"Command Dynamic-link library not found.");
							break;
						case SE_ERR_NOASSOC:
							Logger::Debug(L"Command No program is associated file.");
							break;
						default:
							Logger::Debug(L"Command An unexpected error occured.");
							break;
					}

					if(sei.hProcess)
						::CloseHandle(sei.hProcess);
					return FALSE;
				}

				ret = FALSE;

				if(sei.hProcess)
				{
					if(openas || wait)
					{
						DWORD dwWaitResult = 0;
						// Wait until child process exits.
						// no time-out interval
						dwWaitResult = ::WaitForSingleObject(sei.hProcess, INFINITE);

						if(dwWaitResult == WAIT_OBJECT_0)
						{
							// child process end normal
							//Logger::Debug(L"child process end normal");
						}
					}

					DWORD exit_code = 0; //child process exit code.
					if(::GetExitCodeProcess(sei.hProcess, &exit_code))
					{
						if(exit_code != STILL_ACTIVE)
						{
							Logger::Debug(L"exit code = %d", exit_code);
							ret = exit_code;
						}
					}
					::CloseHandle(sei.hProcess);
				}

				return ret;
			}
		};
	}
}