#include "client.h"
#include "support/log.h"

#include <cassert>
#include <cstdio>

// ideally this would go in beginRpc, but we can't put it there because we
// need to check that the previous command has finished before we start
// manipulating parameters
#define WAIT_FOR_PREVIOUS_COMMAND { \
	if (tryWaitForCompletion() != WakeReason::Complete) { \
		return false; \
	} \
}

namespace IPC {
	Client::Client() :
		m_process(INVALID_HANDLE_VALUE),
		m_sharedMem(INVALID_HANDLE_VALUE),
		m_rpcStartEvent(INVALID_HANDLE_VALUE),
		m_rpcCompleteEvent(INVALID_HANDLE_VALUE),
		m_ipcParameters(nullptr),
		m_isRpcPending(false),
		m_freeVecResultPending(false),
		m_freeVecResultReady(false),
		m_freeVecResultWasFreed(false),
		m_freeVecResultId(InvalidVector),
		m_dynVisResultPending(false),
		m_dynVisResultReady(false),
		m_dynVisResultAccepted(false)
	{}

	Client::~Client() {
		if (m_process != INVALID_HANDLE_VALUE) {
			TerminateProcess(m_process, 0);
			CloseHandle(m_process);
			m_process = INVALID_HANDLE_VALUE;
		}

		if (m_ipcParameters != nullptr) {
			UnmapViewOfFile(m_ipcParameters);
			m_ipcParameters = nullptr;
		}

		CleanupHandle(m_sharedMem);
		CleanupHandle(m_rpcStartEvent);
		CleanupHandle(m_rpcCompleteEvent);
	}

	bool Client::isServerActive() {
		if (m_process != INVALID_HANDLE_VALUE) {
			if (WaitForSingleObject(m_process, 0) == WAIT_OBJECT_0) {
				// the host process was started but is no longer running
				CloseHandle(m_process);
				m_process = INVALID_HANDLE_VALUE;
				return false;
			}

			return true;
		}

		return false;
	}

	bool Client::startServer(const char* executable) {
		STARTUPINFO startupInfo = {};
		PROCESS_INFORMATION processInfo = {};
		char strHandles[256] = { 0, };

		if (isServerActive()) {
			if (!TerminateProcess(m_process, 0)) {
				LOG::winerror("Failed to terminate previous 64-bit host process");
				return false;
			}
			const auto waitResult = WaitForSingleObject(m_process, MaxWait);
			if (waitResult != WAIT_OBJECT_0) {
				if (waitResult == WAIT_FAILED) {
					LOG::winerror("Failed waiting for previous 64-bit host process to terminate");
				} else {
					LOG::logline("Timed out waiting for previous 64-bit host process to terminate");
				}
				return false;
			}
		}
		CleanupHandle(m_process);

		if (m_ipcParameters != nullptr) {
			UnmapViewOfFile(m_ipcParameters);
			m_ipcParameters = nullptr;
		}
		CleanupHandle(m_sharedMem);
		CleanupHandle(m_rpcStartEvent);
		CleanupHandle(m_rpcCompleteEvent);

		// A replaced process is proven unable to consume the old RPC/vector.
		// Reset cached completion state before the new IPC session is created.
		m_isRpcPending = false;
		m_freeVecResultPending = false;
		m_freeVecResultReady = false;
		m_freeVecResultWasFreed = false;
		m_freeVecResultId = InvalidVector;
		m_dynVisResultPending = false;
		m_dynVisResultReady = false;
		m_dynVisResultAccepted = false;
		m_deferredVecFrees.clear();

		// make the mapping handle inheritable
		SECURITY_ATTRIBUTES attrsAllowInherit = {
			sizeof(SECURITY_ATTRIBUTES), NULL, TRUE
		};
		m_sharedMem = CreateFileMappingA(INVALID_HANDLE_VALUE, &attrsAllowInherit, PAGE_READWRITE, 0, sizeof(Parameters), NULL);
		if (m_sharedMem == NULL) {
			LOG::winerror("Failed to create shared memory region");
			goto failedOnCreateMapping;
		}

		m_ipcParameters = static_cast<Parameters*>(MapViewOfFile(m_sharedMem, FILE_MAP_ALL_ACCESS, 0, 0, 0));
		if (m_ipcParameters == nullptr) {
			LOG::winerror("Failed to map shared memory region");
			goto failedOnMap;
		}

		ZeroMemory(m_ipcParameters, sizeof(Parameters));

		// get other handles the server will need
		HANDLE thisProcess;
		if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &thisProcess, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
			LOG::winerror("Failed to duplicate current process handle");
			goto failedOnProcessHandle;
		}

		m_rpcStartEvent = CreateEventA(&attrsAllowInherit, FALSE, FALSE, NULL);
		if (m_rpcStartEvent == NULL) {
			LOG::winerror("Failed to create RPC start event");
			goto failedOnCreateEvent;
		}

		m_rpcCompleteEvent = CreateEventA(&attrsAllowInherit, FALSE, FALSE, NULL);
		if (m_rpcCompleteEvent == NULL) {
			LOG::winerror("Failed to create RPC complete event");
			goto failedOnCreateCompleteEvent;
		}

		std::sprintf(strHandles, "%p %p %p %p", m_sharedMem, thisProcess, m_rpcStartEvent, m_rpcCompleteEvent);
		if (!CreateProcessA(executable, strHandles, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &startupInfo, &processInfo)) {
			LOG::winerror("Failed to start 64-bit host process %s", executable);
			goto failedOnCreateProcess;
		}

		m_process = processInfo.hProcess;
		// don't care about thread handle
		CloseHandle(processInfo.hThread);

		LOG::logline("64-bit host process started (PID %u)", processInfo.dwProcessId);

		// wait for the server to finish bootstrapping
		if (waitForCompletion() == WakeReason::Complete) {
			return true;
		}

		LOG::logline("Failed waiting for 64-bit host process to initialize");

	failedOnCreateProcess:
		CleanupHandle(m_rpcCompleteEvent);
	failedOnCreateCompleteEvent:
		CleanupHandle(m_rpcStartEvent);
	failedOnCreateEvent:
		CloseHandle(thisProcess);
	failedOnProcessHandle:
		UnmapViewOfFile(m_ipcParameters);
		m_ipcParameters = nullptr;
	failedOnMap:
		CloseHandle(m_sharedMem);
	failedOnCreateMapping:
		m_sharedMem = INVALID_HANDLE_VALUE;
		return false;
	}

	bool Client::beginRpc(Command command) {
		if (m_isRpcPending) {
			LOG::logline("Attempted RPC while another RPC was still in progress");
			return false;
		}

		// clear any unprocessed completion events
		ResetEvent(m_rpcCompleteEvent);

		m_ipcParameters->command = command;
		if (!SetEvent(m_rpcStartEvent)) {
			LOG::winerror("Failed to set RPC start event");
			return false;
		}

		m_isRpcPending = true;
		return true;
	}

	bool Client::allocVec(std::size_t elementSize, std::size_t windowSizeInElements, std::size_t maxSizeInElements, std::size_t initialCapacity) {
		WAIT_FOR_PREVIOUS_COMMAND;
		if (!releaseDeferredVecs() || m_freeVecResultPending || !isServerActive()) {
			return false;
		}

		auto& params = m_ipcParameters->params.allocVecParams;
		params.elementSize = elementSize;
		params.windowSizeInElements = windowSizeInElements;
		params.maxCapacityInElements = maxSizeInElements;
		params.initialCapacity = initialCapacity;
		return beginRpc(Command::AllocVec);
	}

	bool Client::freeVec(VecId id) {
		// A timed-out blocking FreeVec remains owned by its original caller. A
		// retry for that ID reuses the pending/cached result; a different free
		// must wait so an allocated slot can never be mistaken for the old ID.
		if (m_freeVecResultPending) {
			return m_freeVecResultId == id;
		}

		WAIT_FOR_PREVIOUS_COMMAND;

		auto& params = m_ipcParameters->params.freeVecParams;
		params.id = id;
		params.wasFreed = false;
		return beginRpc(Command::FreeVec);
	}

	bool Client::awaitFreeVec() {
		if (!m_freeVecResultPending) {
			if (!m_isRpcPending || m_ipcParameters->command != Command::FreeVec) {
				LOG::logline("No vector free RPC result is pending");
				return false;
			}
			m_freeVecResultPending = true;
			m_freeVecResultReady = false;
			m_freeVecResultWasFreed = false;
			m_freeVecResultId = m_ipcParameters->params.freeVecParams.id;
		}

		if (!m_freeVecResultReady) {
			const auto result = waitForCompletion();
			if (result != WakeReason::Complete) {
				if (result == WakeReason::ServerLost) {
					m_freeVecResultPending = false;
					m_freeVecResultReady = false;
					m_freeVecResultWasFreed = false;
					m_freeVecResultId = InvalidVector;
				}
				LOG::logline("Vec free RPC failed");
				return false;
			}
		}

		if (!m_freeVecResultReady) {
			LOG::logline("Vec free RPC completed without a matching result");
			return false;
		}

		const bool wasFreed = m_freeVecResultWasFreed;
		m_freeVecResultPending = false;
		m_freeVecResultReady = false;
		m_freeVecResultWasFreed = false;
		m_freeVecResultId = InvalidVector;
		return wasFreed;
	}

	bool Client::freeVecBlocking(VecId id) {
		if (!freeVec(id)) {
			return false;
		}

		return awaitFreeVec();
	}

	void Client::retainFailedVecAllocation(VecId id) {
		if (id == InvalidVector) {
			return;
		}
		m_deferredVecFrees.push_back(id);
		if (!releaseDeferredVecs()) {
			LOG::logline("Retained failed local mapping for vec %u for cleanup retry", id);
		}
	}

	bool Client::releaseDeferredVecs() {
		auto discardAfterHostLoss = [this]() {
			if (!m_deferredVecFrees.empty()) {
				LOG::logline(
					"Discarding %u deferred vector IDs after IPC host loss",
					static_cast<unsigned>(m_deferredVecFrees.size()));
			}
			m_deferredVecFrees.clear();
			m_freeVecResultPending = false;
			m_freeVecResultReady = false;
			m_freeVecResultWasFreed = false;
			m_freeVecResultId = InvalidVector;
			m_isRpcPending = false;
			return true;
		};

		if (!isServerActive()) {
			return discardAfterHostLoss();
		}

		while (!m_deferredVecFrees.empty()) {
			const auto id = m_deferredVecFrees.back();
			if (freeVecBlocking(id)) {
				m_deferredVecFrees.pop_back();
				continue;
			}
			if (!isServerActive()) {
				return discardAfterHostLoss();
			}
			return false;
		}
		return true;
	}

	bool Client::updateDynVis(VecId id) {
		if (m_dynVisResultPending) {
			return false;
		}
		WAIT_FOR_PREVIOUS_COMMAND;

		auto& params = m_ipcParameters->params.dynVisParams;
		params.id = id;
		params.accepted = false;
		if (!beginRpc(Command::UpdateDynVis)) {
			return false;
		}
		m_dynVisResultPending = true;
		m_dynVisResultReady = false;
		m_dynVisResultAccepted = false;
		return true;
	}

	WakeReason Client::awaitDynVis(bool& accepted) {
		accepted = false;
		if (m_dynVisResultReady) {
			accepted = m_dynVisResultAccepted;
			m_dynVisResultPending = false;
			m_dynVisResultReady = false;
			return WakeReason::Complete;
		}
		if (!m_dynVisResultPending) {
			return WakeReason::Error;
		}

		const auto result = waitForCompletion();
		if (result == WakeReason::Complete) {
			if (!m_dynVisResultReady) {
				return WakeReason::Error;
			}
			accepted = m_dynVisResultAccepted;
			m_dynVisResultPending = false;
			m_dynVisResultReady = false;
		} else if (result == WakeReason::ServerLost) {
			m_dynVisResultPending = false;
			m_dynVisResultReady = false;
		}
		return result;
	}

	bool Client::initDistantStatics(VecId distantStatics, VecId distantSubsets) {
		WAIT_FOR_PREVIOUS_COMMAND;

		auto& params = m_ipcParameters->params.distantStaticParams;
		params.distantStatics = distantStatics;
		params.distantSubsets = distantSubsets;
		return beginRpc(Command::InitDistantStatics);
	}

	bool Client::initLandscape(VecId landscapeBuffers) {
		WAIT_FOR_PREVIOUS_COMMAND;

		m_ipcParameters->params.initLandscapeParams.buffers = landscapeBuffers;
		return beginRpc(Command::InitLandscape);
	}

	bool Client::awaitInitLandscape(bool& outHasCompositeSet, std::uint32_t& outCellCount, std::uint32_t& outDefaultEdgeTexels) {
		assert(m_ipcParameters->command == Command::InitLandscape);

		outHasCompositeSet = false;
		outCellCount = 0;
		outDefaultEdgeTexels = 0;

		if (waitForCompletion() != WakeReason::Complete) {
			LOG::logline("InitLandscape RPC failed");
			return false;
		}

		// The server's Format_Loader wrote these OUT fields from its loaded composite pool
		// (false for Old_Format / absent / malformed). Read them before any further RPC reuses
		// the shared parameter union.
		const auto& params = m_ipcParameters->params.initLandscapeParams;
		outHasCompositeSet = params.hasCompositeSet;
		outCellCount = params.compositeCellCount;
		outDefaultEdgeTexels = params.defaultEdgeTexels;
		return true;
	}

	bool Client::streamVisibleComposites(VecId delta, VecId outHeaders, VecId outBytes) {
		WAIT_FOR_PREVIOUS_COMMAND;

		auto& params = m_ipcParameters->params.streamCompositesParams;
		params.delta = delta;
		params.outHeaders = outHeaders;
		params.outBytes = outBytes;
		return beginRpc(Command::StreamVisibleComposites);
	}

    bool Client::getRetainedWorldCatalogBlocking(
        VecId header,
        VecId cells,
        VecId meshes,
        VecId placements,
        VecId blob) {
        WAIT_FOR_PREVIOUS_COMMAND;

        auto& params = m_ipcParameters->params.retainedCatalogParams;
        params.header = header;
        params.cells = cells;
        params.meshes = meshes;
        params.placements = placements;
        params.blob = blob;
        params.available = false;

        if (!beginRpc(Command::GetRetainedWorldCatalog) ||
            waitForCompletion() != WakeReason::Complete) {
            return false;
        }

        return params.available;
    }

	bool Client::setWorldSpaceBlocking(const std::string& cellname) {
		WAIT_FOR_PREVIOUS_COMMAND;

		auto& params = m_ipcParameters->params.worldSpaceParams;
		strncpy(params.cellname, cellname.c_str(), sizeof(params.cellname));
		params.cellname[sizeof(params.cellname) - 1] = 0;
		if (!beginRpc(Command::SetWorldSpace)) {
			return false;
		}

		if (waitForCompletion() != WakeReason::Complete) {
			return false;
		}

		return params.cellFound;
	}

	bool Client::getVisibleMeshesCoarse(VecId visibleSet, const ViewFrustum& viewFrustum, DWORD setFlags, VisibleSetSort sort) {
		WAIT_FOR_PREVIOUS_COMMAND;

		auto& params = m_ipcParameters->params.meshParams;
		params.visibleSet = visibleSet;
		params.viewFrustum = viewFrustum;
		params.sort = sort;
		params.setFlags = setFlags;
		return beginRpc(Command::GetVisibleMeshesCoarse);
	}

	bool Client::getVisibleMeshes(VecId visibleSet, const ViewFrustum& viewFrustum, const D3DXVECTOR4& viewSphere, DWORD setFlags, VisibleSetSort sort) {
		WAIT_FOR_PREVIOUS_COMMAND;

		auto& params = m_ipcParameters->params.meshParams;
		params.visibleSet = visibleSet;
		params.viewFrustum = viewFrustum;
		params.viewSphere = viewSphere;
		params.sort = sort;
		params.setFlags = setFlags;
		return beginRpc(Command::GetVisibleMeshes);
	}

	bool Client::sortVisibleSet(VecId visibleSet, VisibleSetSort sort) {
		WAIT_FOR_PREVIOUS_COMMAND;

		if (sort == VisibleSetSort::None) {
			SetEvent(m_rpcCompleteEvent);
			return true;
		}

		auto& params = m_ipcParameters->params.meshParams;
		params.visibleSet = visibleSet;
		params.sort = sort;
		return beginRpc(Command::SortVisibleSet);
	}

	WakeReason Client::waitForCompletion(DWORD ms) {
		auto result = WaitForMultipleObjects(2, m_waitHandles, FALSE, ms);
		switch (result) {
		case WAIT_FAILED:
			LOG::winerror("IPC client wait for RPC completion failed");
			return WakeReason::Error;
		case WAIT_TIMEOUT:
			return WakeReason::Timeout;
		default:
			auto handleIndex = result - WAIT_OBJECT_0;
			switch (handleIndex) {
			case 0:
				m_isRpcPending = false;
				m_freeVecResultPending = false;
				m_freeVecResultReady = false;
				m_freeVecResultWasFreed = false;
				m_freeVecResultId = InvalidVector;
				return WakeReason::ServerLost;
			case 1:
				if (m_isRpcPending &&
					m_ipcParameters->command == Command::FreeVec &&
					m_freeVecResultPending &&
					m_freeVecResultId == m_ipcParameters->params.freeVecParams.id) {
					m_freeVecResultWasFreed = m_ipcParameters->params.freeVecParams.wasFreed;
					m_freeVecResultReady = true;
				}
				if (m_isRpcPending &&
					m_ipcParameters->command == Command::UpdateDynVis &&
					m_dynVisResultPending) {
					m_dynVisResultAccepted = m_ipcParameters->params.dynVisParams.accepted;
					m_dynVisResultReady = true;
				}
				m_isRpcPending = false;
				return WakeReason::Complete;
			default:
				return WakeReason::Error;
			}
		}
	}

	WakeReason Client::tryWaitForCompletion(DWORD ms) {
		if (m_isRpcPending) {
			return waitForCompletion(ms);
		}

		return WakeReason::Complete;
	}
}