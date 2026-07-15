#include "ipc/dlshare.h"
#include "ipc/server.h"
#include "mge/dlstreamer.h"
#include "support/log.h"

#include <cassert>
#include <vector>

namespace IPC {
	Server::Server(HANDLE sharedMem, HANDLE clientProcess, HANDLE rpcStartEvent, HANDLE rpcCompleteEvent) :
		m_sharedMem(sharedMem),
		m_clientProcess(clientProcess),
		m_rpcStartEvent(rpcStartEvent),
		m_rpcCompleteEvent(rpcCompleteEvent),
		m_ipcParameters(nullptr),
		m_freeVecs()
	{ }

	Server::~Server() {
		if (m_ipcParameters != nullptr) {
			UnmapViewOfFile(m_ipcParameters);
			m_ipcParameters = nullptr;
		}

		CleanupHandle(m_sharedMem);
		CleanupHandle(m_clientProcess);
		CleanupHandle(m_rpcStartEvent);
		CleanupHandle(m_rpcCompleteEvent);
	}

	bool Server::complete() {
		if (!SetEvent(m_rpcCompleteEvent)) {
			LOG::winerror("Failed to signal RPC completion");
			return false;
		}

		return true;
	}

	bool Server::init() {
		if (m_ipcParameters != nullptr) {
			UnmapViewOfFile(m_ipcParameters);
			m_ipcParameters = nullptr;
		}

		m_ipcParameters = static_cast<Parameters*>(MapViewOfFile(m_sharedMem, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Parameters)));
		if (m_ipcParameters == nullptr) {
			LOG::winerror("Failed to map IPC parameters shared memory");
			return false;
		}

		return true;
	}

	bool Server::listen() {
		while (true) {
			// signal the completion of whatever we were doing before (also signals that we've finished initializing on the first iteration)
			SetEvent(m_rpcCompleteEvent);

			// 0 = client process, 1 = RPC start event
			auto waitResult = WaitForMultipleObjects(2, m_waitHandles, FALSE, INFINITE);
			if (waitResult == WAIT_FAILED) {
				LOG::winerror("Failed to wait for RPC event");
				return false;
			}

			if (waitResult == WAIT_OBJECT_0) {
				LOG::logline("Morrowind process exited; exiting 64-bit host");
				return true;
			}

			switch (m_ipcParameters->command) {
			case Command::None:
				break;
			case Command::AllocVec:
				allocVec();
				break;
			case Command::FreeVec:
				freeVec();
				break;
			case Command::Exit:
				LOG::logline("Host process received exit command");
				return true;
			case Command::UpdateDynVis:
				updateDynVis();
				break;
			case Command::InitDistantStatics:
				initDistantStatics();
				break;
			case Command::InitLandscape:
				initLandscape();
				break;
			case Command::SetWorldSpace:
				setWorldSpace();
				break;
			case Command::GetVisibleMeshesCoarse:
				getVisibleMeshesCoarse();
				break;
			case Command::GetVisibleMeshes:
				getVisibleMeshes();
				break;
			case Command::SortVisibleSet:
				sortVisibleSet();
				break;
			case Command::StreamVisibleComposites:
				streamVisibleComposites();
				break;
            case Command::GetRetainedWorldCatalog:
                getRetainedWorldCatalog();
                break;
			default:
				LOG::logline("Received unknown command value %u", m_ipcParameters->command);
				break;
			}
		}
	}

	template<typename T>
	Vec<T>& Server::getVec(VecId id) {
		auto pVec = m_vecs[id];
		// when the client requests to allocate a shared vector, there's no way we can communicate a template
		// argument to the server, so all vectors are stored with a dummy type of char. the actual contained
		// type doesn't affect the layout of the vector (as all it holds is a pointer to the elements), so
		// we can freely cast between types without breaking the class itself. we will do an assert to make
		// sure the size of the type we're being told the vector contains matches the size of the type it
		// was told it contained when it was created.
		assert(sizeof(T) == pVec->m_elementBytes);
		return *reinterpret_cast<Vec<T>*>(pVec);
	}

	bool Server::allocVec() {
		auto& params = m_ipcParameters->params.allocVecParams;

		Vec<char>* vec = nullptr;
		VecId id = InvalidVector;
		if (!m_freeVecs.empty()) {
			id = m_freeVecs.front();
			m_freeVecs.pop();
			m_vecs[id] = vec = new Vec<char>(id, nullptr, params.maxCapacityInElements, params.windowSizeInElements, params.elementSize);
		} else {
			id = static_cast<VecId>(m_vecs.size());
			vec = new Vec<char>(id, nullptr, params.maxCapacityInElements, params.windowSizeInElements, params.elementSize);
			m_vecs.push_back(vec);
		}

		m_ipcParameters->params.allocVecParams.id = id;

		if (!(vec->init(m_clientProcess, params) && vec->reserve(params.initialCapacity))) {
			delete vec;
			m_vecs[id] = nullptr;
			// mark this slot free again
			m_freeVecs.push(id);
			// The client's awaitAllocVec keys success off params.id != InvalidVector, but id was set
			// above before init ran. On failure we MUST reset it to InvalidVector, otherwise the
			// client believes it owns a channel the server never mapped (silent desync).
			m_ipcParameters->params.allocVecParams.id = InvalidVector;
			return false;
		}

		return true;
	}

	bool Server::freeVec() {
		auto& params = m_ipcParameters->params.freeVecParams;
		params.wasFreed = false;

		if (params.id == InvalidVector) {
			return true;
		}

		auto& vec = m_vecs[params.id];
		if (vec != nullptr) {
			if (!vec->can_free())
				return false;

			delete vec;
			vec = nullptr;
			m_freeVecs.push(params.id);
			params.wasFreed = true;
		}

		return true;
	}

	void Server::updateDynVis() {
		auto& params = m_ipcParameters->params.dynVisParams;
		auto& vec = getVec<DynVisFlag>(params.id);
		for (auto& update : vec) {
			for (auto mesh : DistantLandShare::dynamicVisGroupsServer[update.groupIndex]) {
				mesh->enabled = update.enable;
			}
		}
	}

	bool Server::initDistantStatics() {
		auto& params = m_ipcParameters->params.distantStaticParams;
		auto& distantStatics = getVec<DistantStatic>(params.distantStatics);
		auto& distantSubsets = getVec<DistantSubset>(params.distantSubsets);
		return DistantLandShare::initDistantStaticsServer(distantStatics, distantSubsets);
	}

	bool Server::initLandscape() {
		auto& params = m_ipcParameters->params.initLandscapeParams;
		bool ok = DistantLandShare::initLandscapeServer(getVec<LandscapeBuffers>(params.buffers), params.texWorldColour);

		// Surface the Format_Loader's composite-pool state back to the client (task 5.2
		// additions). hasCompositeSet drives the client's Old_Format vs New_Format path:
		// when false the Composite_Streamer stays inert and the client binds the atlas
		// only (Single_Atlas_Path). These mirror the loaded DistantLandShare::compositePool;
		// for any Old/absent/malformed load the pool is inactive, so hasCompositeSet is
		// false (Requirements 4.1, 4.6, 6.3).
		const auto& pool = DistantLandShare::compositePool;
		params.hasCompositeSet = pool.active;
		params.compositeCellCount = pool.active ? pool.cellCount : 0;
		params.defaultEdgeTexels = pool.active ? pool.defaultEdgeTexels : 0;

		return ok;
	}

	void Server::setWorldSpace() {
		auto& params = m_ipcParameters->params.worldSpaceParams;
		params.cellFound = DistantLandShare::setCurrentWorldSpace(params.cellname);
	}

	void Server::getVisibleMeshesCoarse() {
		auto& params = m_ipcParameters->params.meshParams;
		auto& vec = getVec<RenderMesh>(params.visibleSet);
		DistantLandShare::getVisibleMeshesCoarse(vec, params.viewFrustum, params.sort, params.setFlags);
	}

	void Server::getVisibleMeshes() {
		auto& params = m_ipcParameters->params.meshParams;
		auto& vec = getVec<RenderMesh>(params.visibleSet);
		DistantLandShare::getVisibleMeshes(vec, params.viewFrustum, params.viewSphere, params.sort, params.setFlags);
	}

	void Server::sortVisibleSet() {
		auto& params = m_ipcParameters->params.meshParams;
		auto& vec = getVec<RenderMesh>(params.visibleSet);
		DistantLandShare::sortVisibleSet(vec, params.sort);
	}

	void Server::streamVisibleComposites() {
		// Bridge the on-the-wire delta (a shared Vec<CompositeCellId> the client filled with
		// the cells newly entering the Visible_Cell_Set) into the streamer's pure
		// VisibleCellDelta, then let CompositeStreamerServer fill the header + byte output
		// channels from the loaded composite pool. The streamer is inert when the pool never
		// went active (Old_Format / absent / malformed), leaving both channels empty (Req 4.6).
		auto& params = m_ipcParameters->params.streamCompositesParams;

		auto& deltaVec = getVec<CompositeCellId>(params.delta);
		auto& outHeaders = getVec<CompositeChunkMsg>(params.outHeaders);
		auto& outBytes = getVec<std::uint8_t>(params.outBytes);

		VisibleCellDelta delta;
		delta.reserve(deltaVec.size());
		for (const auto& c : deltaVec) {
			delta.push_back(CellId{ c.cellX, c.cellY });
		}

		CompositeStreamerServer streamer(DistantLandShare::compositePool);
		streamer.streamNewlyVisible(delta, outHeaders, &outBytes);
	}

    void Server::getRetainedWorldCatalog() {
        auto& params = m_ipcParameters->params.retainedCatalogParams;
        params.available = DistantLandShare::writeRetainedCatalog(
            getVec<RetainedCatalog::Header>(params.header),
            getVec<RetainedCatalog::Cell>(params.cells),
            getVec<RetainedCatalog::Mesh>(params.meshes),
            getVec<RetainedCatalog::Placement>(params.placements),
            getVec<std::uint8_t>(params.blob));
    }
}