#pragma once

#include "support/winheader.h"
#include "ipc/bridge.h"
#include "ipc/view.h"

#include <cassert>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace IPC {
	/**
	* @class Client
	* @brief An interface to the 64-bit server for the 32-bit client.
	* 
	* Client provides an interface to RPC to the server process. The current main purpose of the server is to host the
	* distant land QuadTrees and search them for visible meshes, providing the visible set to the client. To support
	* this, the server provides an API to allocate shared vectors in which the visible sets (or other data) can be
	* stored and shared with the client.
	* 
	* Because rendering is done on the client side, D3D resources must be allocated on the client side. However, the
	* server's distant land structures contain references to these resources. The server therefore provides APIs for
	* the client to push resource information to the server during distant land initialization. The remainder of the
	* API deals with controlling settings related to visible distant meshes and searching the QuadTrees for currently
	* visible meshes.
	* 
	* The server process must be started by the client via the startServer method. This method blocks until the server
	* signals that it has finished initializing and is ready to accept RPCs.
	* 
	* To provide opportunities for parallelism, all RPC methods are asynchronous except the ones that contain the word
	* "blocking". For asynchronous calls that return a value, there will be a corresponding `await` method that will
	* block until the result is available and return it. For asynchronous calls that don't return a value, you can use
	* the generic `waitForCompletion` method to wait for the RPC to complete. The blocking methods will return the
	* result directly.
	* 
	* Only one RPC may be active at a time. If an attempt is made to start another RPC while a previous one is still
	* active, the new RPC will block until the previous RPC is complete. This means that it's not necessary to
	* explicitly wait for RPC completion if you don't care about the results. You can fire off an asynchronous call and
	* move on, and the next RPC request will block if necessary before starting.
	*/
	class Client {
		HANDLE m_sharedMem;
		HANDLE m_rpcStartEvent;
		union {
			struct {
				HANDLE m_process;
				HANDLE m_rpcCompleteEvent;
			};
			HANDLE m_waitHandles[2];
		};
		Parameters* m_ipcParameters;
		std::uint64_t m_sessionGeneration;
		bool m_isRpcPending;
		bool m_freeVecResultPending;
		bool m_freeVecResultReady;
		bool m_freeVecResultWasFreed;
		VecId m_freeVecResultId;
		bool m_dynVisResultPending;
		bool m_dynVisResultReady;
		bool m_dynVisResultAccepted;
        bool m_compositeStreamResultReady;
        CompositeBatchStatus m_compositeStreamResult;
		std::vector<VecId> m_deferredVecFrees;

		bool beginRpc(Command command);
		WakeReason tryWaitForCompletion(DWORD ms = MaxWait);
		void retainFailedVecAllocation(VecId id);

	public:
		Client();
		~Client();
		Client(const Client&) = delete;
		Client& operator=(const Client&) = delete;

		bool startServer(const char* executable);
		bool isServerActive();
        std::uint64_t sessionGeneration() const { return m_sessionGeneration; }

		/**
		* @brief Asynchronously allocate a shared vector.
		* @param elementSize The size in bytes of the type of element that will be stored in the vector.
		* @param windowSizeInElements The number of elements that will be visible in a single window of the client view.
		*                             The size of the window will be rounded up to a multiple of the system allocation granularity.
		* @param maxSizeInElements The maximum number of elements to reserve memory for in the vector. The size of the
		*                          reservation will be rounded up to a multiple of the system allocation granularity.
		* @param initialCapacity Number of elements to initially commit memory for.
		* @return Whether the RPC was issued successfully.
		*/
		bool allocVec(std::size_t elementSize, std::size_t windowSizeInElements, std::size_t maxSizeInElements, std::size_t initialCapacity);

		/**
		* @brief Await the result of a previous asynchronous vector allocation.
		* @return A VecView with a view into the vector on success, or std::nullopt on failure.
		*/
		template<typename T>
		std::optional<VecView<T>> awaitAllocVec() {
			assert(m_ipcParameters->command == Command::AllocVec);

			auto result = waitForCompletion();
			if (result != WakeReason::Complete) {
				LOG::logline("Vec allocation RPC failed");
				return std::nullopt;
			}

			auto& params = m_ipcParameters->params.allocVecParams;
			if (params.id == InvalidVector) {
				LOG::logline("Vec allocation rejected by server");
				return std::nullopt;
			}
			const auto allocatedId = params.id;

			assert(sizeof(T) == params.elementSize);

			// map header
			auto header32 = static_cast<VecBase::VecShare*>(MapViewOfFile(params.sharedMem32, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(VecBase::VecShare)));
			if (header32 == nullptr) {
				LOG::winerror("Failed to map header for vec %u", allocatedId);
				retainFailedVecAllocation(allocatedId);
				return std::nullopt;
			}

			// recalculate elements per window
			auto windowElements = params.windowBytes / sizeof(T);
			VecView<T> view(allocatedId, header32, static_cast<std::size_t>(windowElements), static_cast<std::size_t>(params.windowBytes),
				static_cast<std::size_t>((params.reservedBytes / params.windowBytes) * windowElements), static_cast<std::size_t>(params.reservedBytes), static_cast<std::size_t>(params.headerBytes));
			if (!view.init()) {
				// Drop the client view (and users32 reference) before asking the
				// server to free the allocation. The client retains the ID if the
				// free cannot complete while the host is still alive.
				view = VecView<T>();
				retainFailedVecAllocation(allocatedId);
				return std::nullopt;
			}

			return view;
		}

		/**
		* @brief Synchronously allocate a shared vector.
		* @param windowSizeInElements The number of elements that will be visible in a single window of the client view.
		*                             The size of the window will be rounded up to a multiple of the system allocation granularity.
		* @param maxSizeInElements The maximum number of elements to reserve memory for in the vector. The size of the
		*                          reservation will be rounded up to a multiple of the system allocation granularity.
		* @param initialCapacity Number of elements to initially commit memory for.
		* @return A VecView with a view into the vector on success, or std::nullopt on failure.
		*/
		template<typename T>
		std::optional<VecView<T>> allocVecBlocking(std::size_t windowSizeInElements, std::size_t maxSizeInElements, std::size_t initialCapacity) {
			if (!allocVec(sizeof(T), windowSizeInElements, maxSizeInElements, initialCapacity)) {
				return std::nullopt;
			}

			return awaitAllocVec<T>();
		}

		/**
		* @brief Asynchronously deallocate a shared vector.
		* 
		* Make sure any VecViews of the vector are destroyed prior to this call, otherwise the deallocation will fail.
		* 
		* @param id The ID of the vector to deallocate.
		* @return Whether the RPC was issued successfully.
		*/
		bool freeVec(VecId id);

		/**
		* @brief Await the result of a previous vector deallocation request.
		* @return Whether the vector was actually freed.
		*/
		bool awaitFreeVec();

		/**
		* @brief Synchronously deallocate a shared vector.
		*
		* Make sure any VecViews of the vector are destroyed prior to this call, otherwise the deallocation will fail.
		*
		* @param id The ID of the vector to deallocate.
		* @return Whether the vector was actually freed.
		*/
		bool freeVecBlocking(VecId id);

		/**
		* @brief Retry vector frees retained after local allocation-view failures.
		* @return True when all deferred vectors were freed, or their server is
		*         proven gone and therefore no longer owns them.
		*/
		bool releaseDeferredVecs();

		/**
		* @brief Asynchronously update mesh dynamic visibility flags.
		* @param flags ID of a shared vector which the client has filled with flags to be updated.
		* @return Whether the RPC was issued successfully.
		*/
		bool updateDynVis(VecId flags);

		/**
		* @brief Await host acknowledgment of a previously-issued dynamic visibility update.
		* @param accepted Set only when the host consumed a valid update vector.
		* @return Completion, timeout, or host-loss reason for preserving retry ownership.
		*/
		WakeReason awaitDynVis(bool& accepted);

		/**
		* @brief Inform the server of distant static D3D resources.
		* @param distantStatics ID of a shared vector of DistantStatic objects.
		* @param distantSubsets ID of a shared vector of DistantSubset objects.
		* @return Whether the RPC was issued successfully.
		*/
		bool initDistantStatics(VecId distantStatics, VecId distantSubsets);

		/**
		* @brief Inform the server of distant landscape D3D resources.
		* @param landscapeBuffers ID of a shared vector of LandscapeBuffers objects. The server expects to process this
		*                         vector in parallel with the client, so the client must use start_write/end_write and
		*                         begin filling the vector after calling this method.
		* @return Whether the RPC was issued successfully.
		*/
		bool initLandscape(VecId landscapeBuffers);

		/**
		* @brief Await completion of a previously-issued InitLandscape RPC and read back the
		*        composite-pool shape the server's Format_Loader reported (Architecture B).
		*
		* The server fills the InitLandscape OUT fields (hasCompositeSet / compositeCellCount /
		* defaultEdgeTexels) from its loaded composite pool. They are only valid once the RPC has
		* completed, and they share a union with every other command's parameters, so this MUST be
		* called after the landscape buffers are fully written (end_write) and BEFORE any further
		* RPC (e.g. freeVec) overwrites the parameter block. On a non-composite / Old_Format load
		* the server reports hasCompositeSet=false and the client stays on the Single_Atlas_Path.
		*
		* @param outHasCompositeSet  Set to the server's New_Format verdict (false => Old_Format).
		* @param outCellCount        Number of cells in the server pool (0 when inert).
		* @param outDefaultEdgeTexels Server's default per-cell baked resolution (0 when inert).
		* @return Whether the RPC completed successfully.
		*/
		bool awaitInitLandscape(bool& outHasCompositeSet, std::uint32_t& outCellCount, std::uint32_t& outDefaultEdgeTexels);

		/**
		* @brief Asynchronously request the composites for newly-visible cells (Composite_Streamer).
		*
		* The client fills @p delta (a Vec<CompositeCellId> of the cells newly entering the
		* Visible_Cell_Set this frame, i.e. newVisible \ alreadyResident) and the server appends
		* one CompositeChunkMsg header per resolved cell to @p outHeaders plus that cell's
		* compressed DXT1 + mip blob to @p outBytes, in matching order. For an Old_Format / inert
		* pool the server leaves both output channels empty. Wait with @ref waitForCompletion
		* before reading the OUT vectors.
		*
		* @param delta      ID of a Vec<CompositeCellId> the client filled with newly-visible cells.
		* @param outHeaders ID of a Vec<CompositeChunkMsg> the server fills (one per streamed cell).
		* @param outBytes   ID of a Vec<uint8_t> the server fills (concatenated DXT1 blobs).
		* @return Whether the RPC was issued successfully. This low-priority call never waits
		*         for another command; false leaves all vector ownership with the current RPC.
		*/
		bool streamVisibleComposites(VecId delta, VecId outHeaders, VecId outBytes);

        // Consume the batch result captured when the composite RPC completed. The result is
        // retained even if another IPC command subsequently reuses the shared parameter union.
        bool takeCompositeStreamResult(CompositeBatchStatus& result);

        /**
         * @brief Populate a terrain/non-grass-static retained-world catalog when changed.
         *
         * All output vectors are caller-allocated. The call blocks until the host has
         * compared generations. When @p unchanged is true the host did not rewrite the
         * vectors, avoiding a periodic full catalog rebuild/copy/hash on the render thread.
         */
        bool getRetainedWorldCatalogBlocking(
            VecId header,
            VecId cells,
            VecId meshes,
            VecId placements,
            VecId blob,
            std::uint64_t knownGeneration,
            bool& unchanged);

		/**
		* @brief Update the current worldspace by informing the server of the player's current cell.
		* @param cellname The name of the player's current cell.
		* @return Whether the RPC was successful.
		*/
		bool setWorldSpaceBlocking(const std::string& cellname);

		/**
		* @brief Asynchronously do a coarse search for visible meshes.
		* @param visibleSet ID of a shared vector of RenderMesh objects which will be populated with the results of the search.
		* @param viewFrustum The camera's current view frustum.
		* @param setFlags Flags indicating which types of meshes to search for. One or more of VIS_NEAR, VIS_FAR, VIS_VERY_FAR,
		*                 VIS_STATIC (= all 3 of the preceding flags), VIS_GRASS, or VIS_LAND.
		* @param sort The desired sorting of the result set, if any. If no sorting is requested (the default), the server will
		*             enable parallel writing on the vector, allowing the client to iterate over the results as they're
		*             populated, if desired (@ref VecView::start_read).
		* @return Whether the RPC was issued successfully.
		*/
		bool getVisibleMeshesCoarse(VecId visibleSet, const ViewFrustum& viewFrustum, DWORD setFlags, VisibleSetSort sort = VisibleSetSort::None);

		/**
		* @brief Asynchronously search for visible meshes.
		* @param visibleSet ID of a shared vector of RenderMesh objects which will be populated with the results of the search.
		* @param viewFrustum The camera's current view frustum.
		* @param viewSphere A sphere defining the region within the draw distance.
		* @param setFlags Flags indicating which types of meshes to search for. One or more of VIS_NEAR, VIS_FAR, VIS_VERY_FAR,
		*                 VIS_STATIC (= all 3 of the preceding flags), VIS_GRASS, or VIS_LAND.
		* @param sort The desired sorting of the result set, if any. If no sorting is requested (the default), the server will
		*             enable parallel writing on the vector, allowing the client to iterate over the results as they're
		*             populated, if desired (@ref VecView::start_read).
		* @return Whether the RPC was issued successfully.
		*/
		bool getVisibleMeshes(VecId visibleSet, const ViewFrustum& viewFrustum, const D3DXVECTOR4& viewSphere, DWORD setFlags, VisibleSetSort sort = VisibleSetSort::None);

		/**
		* @brief Asynchronously sort an already-populated visible set.
		* @param visibleSet ID of a filled shared vector of RenderMesh objects.
		* @param sort The type of sort desired.
		* @return Whether the RPC was issued successfully.
		*/
		bool sortVisibleSet(VecId visibleSet, VisibleSetSort sort);

		WakeReason waitForCompletion(DWORD ms = MaxWait);

        // Non-blocking ownership poll used by callers that retain an in-flight batch after a
        // timeout. Complete means the command no longer owns its shared vectors.
        WakeReason pollForCompletion(DWORD ms = 0) { return tryWaitForCompletion(ms); }
	};
}