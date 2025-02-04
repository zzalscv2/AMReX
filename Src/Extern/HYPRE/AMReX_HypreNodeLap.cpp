#include <AMReX_HypreNodeLap.H>
#include <AMReX_VisMF.H>
#include <AMReX_MLNodeLaplacian.H>

#ifdef AMREX_USE_EB
#include <AMReX_EBMultiFabUtil.H>
#include <AMReX_MultiCutFab.H>
#include <AMReX_EBFabFactory.H>
#endif

#include <cmath>
#include <numeric>
#include <limits>
#include <type_traits>

namespace amrex {

HypreNodeLap::HypreNodeLap (const BoxArray& grids_, const DistributionMapping& dmap_,
                            const Geometry& geom_, const FabFactory<FArrayBox>& factory_,
                            const iMultiFab& owner_mask_, const iMultiFab& dirichlet_mask_,
                            MPI_Comm comm_, MLNodeLinOp const* linop_, int verbose_,
                            const std::string& options_namespace_)
    : grids(grids_), dmap(dmap_), geom(geom_), factory(&factory_),
      owner_mask(&owner_mask_), dirichlet_mask(&dirichlet_mask_),
      comm(comm_), linop(linop_), verbose(verbose_),
      options_namespace(options_namespace_)
{
    Gpu::LaunchSafeGuard lsg(false); // xxxxx TODO: gpu

    static_assert(AMREX_SPACEDIM > 1, "HypreNodeLap: 1D not supported");
    static_assert(std::is_same<Real, HYPRE_Real>::value, "amrex::Real != HYPRE_Real");

    int num_procs, myid;
    MPI_Comm_size(comm, &num_procs);
    MPI_Comm_rank(comm, &myid);

    const BoxArray& nba = amrex::convert(grids,IntVect::TheNodeVector());

#if defined(AMREX_DEBUG) || defined(AMREX_TESTING)
    if (sizeof(Int) < sizeof(Long)) {
        Long nnodes_grids = nba.numPts();
        AMREX_ALWAYS_ASSERT_WITH_MESSAGE(nnodes_grids < static_cast<Long>(std::numeric_limits<Int>::max()),
                                         "You might need to configure Hypre with --enable-bigint");
    }
#endif

    // how many non-covered nodes do we have?
    nnodes_grid.define(grids,dmap);
    node_id.define(nba,dmap,1,1);
    node_id_vec.define(grids,dmap);
    tmpsoln.define(nba,dmap,1,0);

    node_id.setVal(std::numeric_limits<Int>::lowest());

    Int nnodes_proc = 0;

#ifdef AMREX_USE_EB
    auto ebfactory = dynamic_cast<EBFArrayBoxFactory const*>(factory);
    if (ebfactory)
    {
        const FabArray<EBCellFlagFab>& flags = ebfactory->getMultiEBCellFlagFab();
#ifdef AMREX_USE_OMP
#pragma omp parallel reduction(+:nnodes_proc)
#endif
        for (MFIter mfi(node_id); mfi.isValid(); ++mfi)
        {
            const Box& ndbx = mfi.validbox();
            const auto& nid = node_id.array(mfi);
            const auto& flag = flags.array(mfi);
            const auto& owner = owner_mask->array(mfi);
            const auto& dirichlet = dirichlet_mask->array(mfi);
            int id = 0;
            const auto lo = amrex::lbound(ndbx);
            const auto hi = amrex::ubound(ndbx);
            for         (int k = lo.z; k <= hi.z; ++k) {
                for     (int j = lo.y; j <= hi.y; ++j) {
                    for (int i = lo.x; i <= hi.x; ++i) {
                        if (!owner(i,j,k) || dirichlet(i,j,k))
                        {
                            nid(i,j,k) = std::numeric_limits<Int>::lowest();
                        }
#if (AMREX_SPACEDIM == 2)
                        else if (flag(i-1,j-1,k).isCovered() &&
                                 flag(i  ,j-1,k).isCovered() &&
                                 flag(i-1,j  ,k).isCovered() &&
                                 flag(i  ,j  ,k).isCovered())
#endif
#if (AMREX_SPACEDIM == 3)
                        else if (flag(i-1,j-1,k-1).isCovered() &&
                                 flag(i  ,j-1,k-1).isCovered() &&
                                 flag(i-1,j  ,k-1).isCovered() &&
                                 flag(i  ,j  ,k-1).isCovered() &&
                                 flag(i-1,j-1,k  ).isCovered() &&
                                 flag(i  ,j-1,k  ).isCovered() &&
                                 flag(i-1,j  ,k  ).isCovered() &&
                                 flag(i  ,j  ,k  ).isCovered())
#endif
                        {
                            nid(i,j,k) = std::numeric_limits<Int>::lowest();
                        }
                        else
                        {
                            nid(i,j,k) = id++;
                        }
                    }
                }
            }
            nnodes_grid[mfi] = id;
            nnodes_proc += id;
        }
    }
    else
#endif
    {
#ifdef AMREX_USE_OMP
#pragma omp parallel reduction(+:nnodes_proc)
#endif
        for (MFIter mfi(node_id); mfi.isValid(); ++mfi)
        {
            const Box& ndbx = mfi.validbox();
            const auto& nid = node_id.array(mfi);
            const auto& owner = owner_mask->array(mfi);
            const auto& dirichlet = dirichlet_mask->array(mfi);
            int id = 0;
            const auto lo = amrex::lbound(ndbx);
            const auto hi = amrex::ubound(ndbx);
            for         (int k = lo.z; k <= hi.z; ++k) {
                for     (int j = lo.y; j <= hi.y; ++j) {
                    for (int i = lo.x; i <= hi.x; ++i) {
                        if (!owner(i,j,k) || dirichlet(i,j,k))
                        {
                            nid(i,j,k) = std::numeric_limits<Int>::lowest();
                        }
                        else
                        {
                            nid(i,j,k) = id++;
                        }
                    }
                }
            }
            nnodes_grid[mfi] = id;
            nnodes_proc += id;
        }
    }

    Vector<Int> nnodes_allprocs(num_procs);
    MPI_Allgather(&nnodes_proc, sizeof(Int), MPI_CHAR,
                  nnodes_allprocs.data(), sizeof(Int), MPI_CHAR,
                  comm);
    Int proc_begin = 0;
    for (int i = 0; i < myid; ++i) {
        proc_begin += nnodes_allprocs[i];
    }

#ifdef AMREX_DEBUG
    Int nnodes_total = 0;
    for (auto n : nnodes_allprocs) {
        nnodes_total += n;
    }
#endif

    LayoutData<Int> offset(grids,dmap);
    Int proc_end = proc_begin;
    for (MFIter mfi(nnodes_grid); mfi.isValid(); ++mfi)
    {
        offset[mfi] = proc_end;
        proc_end += nnodes_grid[mfi];
    }
    AMREX_ALWAYS_ASSERT_WITH_MESSAGE(proc_end == proc_begin+nnodes_proc,
                                     "HypreNodeLap: how did this happen?");

#ifdef AMREX_USE_OMP
#pragma omp parallel if (Gpu::notInLaunchRegion())
#endif

    fill_node_id(offset);

    amrex::OverrideSync(node_id, *owner_mask, geom.periodicity());
    node_id.FillBoundary(geom.periodicity());

    // Create and initialize A, b & x
    Int ilower = proc_begin;
    Int iupper = proc_end-1;

    hypre_ij = std::make_unique<HypreIJIface>(comm, ilower, iupper, verbose);
    hypre_ij->parse_inputs(options_namespace);

    // Obtain non-owning references to the matrix, rhs, and solution data
    A = hypre_ij->A();
    b = hypre_ij->b();
    x = hypre_ij->x();

    Vector<Int> ncols;
    Vector<Int> cols;
    Vector<Real> mat;
    constexpr Int max_stencil_size = AMREX_D_TERM(3,*3,*3);

    for (MFIter mfi(node_id); mfi.isValid(); ++mfi)
    {
        const Int nrows = nnodes_grid[mfi];
        if (nrows > 0)
        {
            ncols.clear();
            ncols.reserve(nrows);

            Vector<Int>& rows = node_id_vec[mfi];
            rows.clear();
            rows.reserve(nrows);

            cols.clear();
            cols.reserve(nrows*max_stencil_size);

            mat.clear();
            mat.reserve(nrows*max_stencil_size);

            const Array4<Int const> nid = node_id.array(mfi);
            const auto& owner = owner_mask->array(mfi);

            linop->fillIJMatrix(mfi, nid, owner, ncols, rows, cols, mat);

#ifdef AMREX_DEBUG
            Int nvalues = 0;
            for (Int i = 0; i < nrows; ++i) {
                nvalues += ncols[i];
            }
            for (Int i = 0; i < nvalues; ++i) {
                AMREX_ASSERT(cols[i] >= 0 && cols[i] < nnodes_total);
            }
#endif

            if (hypre_ij->adjustSingularMatrix()
                && linop->isBottomSingular()
                && (rows[0] == 0)) {
                const int num_cols = ncols[0];
                for (int ic = 0; ic < num_cols; ++ic)
                    mat[ic] = (cols[ic] == rows[0]) ? mat[ic] : 0.0;
            }

            HYPRE_IJMatrixSetValues(A, nrows, ncols.data(), rows.data(), cols.data(), mat.data());
        }
    }
    HYPRE_IJMatrixAssemble(A);
}

HypreNodeLap::~HypreNodeLap ()
{}

void
HypreNodeLap::solve (MultiFab& soln, const MultiFab& rhs,
                     Real rel_tol, Real abs_tol, int max_iter)
{
    BL_PROFILE("HypreNodeLap::solve()");

    HYPRE_IJVectorInitialize(b);
    HYPRE_IJVectorInitialize(x);
    //
    loadVectors(soln, rhs);
    //
    HYPRE_IJVectorAssemble(x);
    HYPRE_IJVectorAssemble(b);

    hypre_ij->solve(rel_tol, abs_tol, max_iter);

    getSolution(soln);
}

void
HypreNodeLap::fill_node_id (LayoutData<Int>& offset)
{
    for (MFIter mfi(node_id,TilingIfNotGPU()); mfi.isValid(); ++mfi)
    {
        Int os = offset[mfi];
        const Box& bx = mfi.growntilebox();
        const auto& nid = node_id.array(mfi);
        AMREX_HOST_DEVICE_PARALLEL_FOR_3D(bx, i, j, k,
        {
            if (nid(i,j,k) >= 0) {
                nid(i,j,k) += os;
            } else {
                nid(i,j,k) = -1;
            }
        });
    }
}

void
HypreNodeLap::loadVectors (MultiFab& soln, const MultiFab& rhs)
{
    BL_PROFILE("HypreNodeLap::loadVectors()");

    soln.setVal(0.0);

    Vector<Real> bvec;
    for (MFIter mfi(soln); mfi.isValid(); ++mfi)
    {
        const Int nrows = nnodes_grid[mfi];
        if (nrows >= 0)
        {
            const Vector<Int>& rows = node_id_vec[mfi];
            HYPRE_IJVectorSetValues(x, nrows, rows.data(), soln[mfi].dataPtr());

            bvec.clear();
            bvec.reserve(nrows);

            const Box& bx = mfi.validbox();
            const auto lo = amrex::lbound(bx);
            const auto hi = amrex::ubound(bx);
            const auto& bfab = rhs.array(mfi);
            const auto& nid = node_id.array(mfi);
            const auto& owner = owner_mask->array(mfi);
            for         (int k = lo.z; k <= hi.z; ++k) {
                for     (int j = lo.y; j <= hi.y; ++j) {
                    for (int i = lo.x; i <= hi.x; ++i) {
                        if (nid(i,j,k) >= 0 && owner(i,j,k)) {
                            bvec.push_back(bfab(i,j,k));
                        }
                    }
                }
            }

            if (hypre_ij->adjustSingularMatrix()
                && linop->isBottomSingular()
                && (rows[0] == 0)) {
                bvec[0] = 0.0;
            }

            HYPRE_IJVectorSetValues(b, nrows, rows.data(), bvec.data());
        }
    }
}

void
HypreNodeLap::getSolution (MultiFab& soln)
{
    tmpsoln.setVal(0.0);

    Vector<Real> xvec;
    for (MFIter mfi(tmpsoln); mfi.isValid(); ++mfi)
    {
        const Int nrows = nnodes_grid[mfi];
        if (nrows >= 0)
        {
            const Vector<Int>& rows = node_id_vec[mfi];
            xvec.resize(nrows);
            HYPRE_IJVectorGetValues(x, nrows, rows.data(), xvec.data());

            const Box& bx = mfi.validbox();
            const auto lo = amrex::lbound(bx);
            const auto hi = amrex::ubound(bx);
            const auto& xfab = tmpsoln.array(mfi);
            const auto& nid = node_id.array(mfi);
            const auto& owner = owner_mask->array(mfi);
            int offset = 0;
            for         (int k = lo.z; k <= hi.z; ++k) {
                for     (int j = lo.y; j <= hi.y; ++j) {
                    for (int i = lo.x; i <= hi.x; ++i) {
                        if (nid(i,j,k) >= 0 && owner(i,j,k)) {
                            xfab(i,j,k) = xvec[offset++];
                        }
                    }
                }
            }
        }
    }

    soln.ParallelAdd(tmpsoln, 0, 0, 1, geom.periodicity());
}

}
