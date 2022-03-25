#include "diffusion.hpp"
#include "util.hpp"

#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/utility/setup.hpp>

#include "cam_timers.hpp"

#define LOG BOOST_LOG_TRIVIAL(debug)

#define NCOL 11
#define GET(X, i, j) X[i * NCOL + j]

inline int read_mesh(adios2::ADIOS *ad, adios2::IO &io, std::string xgcdir)
{
    int n_t;
    adios2::Engine reader;
    io = ad->DeclareIO("diagnosis.mesh");

    boost::filesystem::path fname = boost::filesystem::path(xgcdir) / boost::filesystem::path("xgc.mesh.bp");
    LOG << "Loading: " << fname;
    reader = io.Open(fname.string(), adios2::Mode::Read, MPI_COMM_SELF);
    reader.BeginStep();
    reader.Get<int>("n_t", &n_t);
    reader.EndStep();
    reader.Close();

    return n_t;
}

inline double vec_sum(std::vector<double> &vec)
{
    double sum = 0;
    for (auto &x : vec)
        sum += x;
    return sum;
}

void Diffusion::reset()
{
    this->i_dr_avg.resize(this->ntriangle);
    this->i_dr_squared_average.resize(this->ntriangle);
    this->i_dE_avg.resize(this->ntriangle);
    this->i_dE_squared_average.resize(this->ntriangle);
    this->i_marker_den.resize(this->ntriangle);
    this->e_dr_avg.resize(this->ntriangle);
    this->e_dr_squared_average.resize(this->ntriangle);
    this->e_dE_avg.resize(this->ntriangle);
    this->e_dE_squared_average.resize(this->ntriangle);
    this->e_marker_den.resize(this->ntriangle);

    std::fill(this->i_dr_avg.begin(), this->i_dr_avg.end(), 0.0);
    std::fill(this->i_dr_squared_average.begin(), this->i_dr_squared_average.end(), 0.0);
    std::fill(this->i_dE_avg.begin(), this->i_dE_avg.end(), 0.0);
    std::fill(this->i_dE_squared_average.begin(), this->i_dE_squared_average.end(), 0.0);
    std::fill(this->i_marker_den.begin(), this->i_marker_den.end(), 0.0);
    std::fill(this->e_dr_avg.begin(), this->e_dr_avg.end(), 0.0);
    std::fill(this->e_dr_squared_average.begin(), this->e_dr_squared_average.end(), 0.0);
    std::fill(this->e_dE_avg.begin(), this->e_dE_avg.end(), 0.0);
    std::fill(this->e_dE_squared_average.begin(), this->e_dE_squared_average.end(), 0.0);
    std::fill(this->e_marker_den.begin(), this->e_marker_den.end(), 0.0);
}

Diffusion::Diffusion(adios2::ADIOS *ad, std::string xgcdir, MPI_Comm comm)
{
    TIMER_START("INIT");
    this->ad = ad;
    this->xgcdir = xgcdir;

    this->comm = comm;
    MPI_Comm_rank(comm, &this->rank);
    MPI_Comm_size(comm, &this->comm_size);

    this->ntriangle = read_mesh(ad, this->io, this->xgcdir);
    this->istep = 0;

    this->reset();

    this->io = ad->DeclareIO("tracer_diag");
    boost::filesystem::path fname =
        boost::filesystem::path(this->xgcdir) / boost::filesystem::path("xgc.tracer_diag.bp");
    LOG << "Loading: " << fname;
    this->reader = this->io.Open(fname.string(), adios2::Mode::Read, this->comm);

    this->dup_io = ad->DeclareIO("tracer_diag_dup");
    this->dup_io.DefineVariable<double>("table", {}, {}, {0, NCOL});
    this->dup_writer = this->io.Open("xgc.tracer_diag.bp.copy", adios2::Mode::Write, this->comm);
    TIMER_STOP("INIT");
}

void Diffusion::finalize()
{
    TIMER_START("FINALIZE");
    this->reader.Close();
    if (this->rank == 0)
        this->writer.Close();
    this->dup_writer.Close();
    TIMER_STOP("FINALIZE");
}

void Diffusion::vec_reduce(std::vector<double> &vec)
{
    if (this->rank == 0)
    {
        MPI_Reduce(MPI_IN_PLACE, vec.data(), vec.size(), MPI_DOUBLE, MPI_SUM, 0, this->comm);
    }
    else
    {
        MPI_Reduce(vec.data(), vec.data(), vec.size(), MPI_DOUBLE, MPI_SUM, 0, this->comm);
    }
}

adios2::StepStatus Diffusion::step()
{
    int total_nrow = 0;

    TIMER_START("STEP");
    TIMER_START("ADIOS_STEP");
    adios2::StepStatus status = this->reader.BeginStep();
    this->dup_writer.BeginStep();
    if (status == adios2::StepStatus::OK)
    {
        this->reset();

        auto var_table = this->io.InquireVariable<double>("table");
        auto block_list = reader.BlocksInfo(var_table, this->istep);

        auto slice = split_vector(block_list, this->comm_size, this->rank);
        LOG << boost::format("Step %d: diffusion offset,nblock= %d %d") % this->istep % slice.first % slice.second;

        int offset = slice.first;
        int nblock = slice.second;

        // Read table block by block
        for (int i = offset; i < offset + nblock; i++)
        {
            auto block = block_list[i];
            std::vector<double> table;

            int ncount = 1;
            for (auto &d : block.Count)
            {
                ncount *= d;
            }

            if (ncount > 0)
            {
                var_table.SetBlockSelection(block.BlockID);
                this->reader.Get<double>(var_table, table);
                TIMER_START("ADIOS_PERFORM_GETS");
                this->reader.PerformGets();
                TIMER_STOP("ADIOS_PERFORM_GETS");

                TIMER_START("_ADIOS_DUP_WRITE");
                auto var = this->dup_io.InquireVariable<double>("table");
                var.SetSelection({{}, block.Count});
                this->dup_writer.Put<double>("table", table.data(), adios2::Mode::Sync);
                TIMER_STOP("_ADIOS_DUP_WRITE");
            }

            // Process each row:
            // Each row of the "table" contains the following info:
            // triangle, i_dr_average, i_dr_squared_average, i_dE_average, i_dE_squared_average, i_marker_den,
            // e_dr_average, e_dr_squared_average, e_dE_average, e_dE_squared_average, e_marker_den
            int nrow = table.size() / NCOL;
            total_nrow += nrow;
            // LOG << "table id,nrow: " << block.BlockID << " " << nrow;
            for (int k = 0; k < nrow; k++)
            {
                int itri = int(GET(table, k, 0));
                double _i_dr_average = GET(table, k, 1);
                double _i_dr_squared_average = GET(table, k, 2);
                double _i_dE_average = GET(table, k, 3);
                double _i_dE_squared_average = GET(table, k, 4);
                double _i_marker_den = GET(table, k, 5);

                double _e_dr_average = GET(table, k, 6);
                double _e_dr_squared_average = GET(table, k, 7);
                double _e_dE_average = GET(table, k, 8);
                double _e_dE_squared_average = GET(table, k, 9);
                double _e_marker_den = GET(table, k, 10);

                // LOG << boost::format("%d: %d %g %g") % block.BlockID % itri % _i_dr_average % _i_marker_den;
                this->i_dr_avg[itri] += _i_dr_average;
                this->i_dr_squared_average[itri] += _i_dr_squared_average;
                this->i_dE_avg[itri] += _i_dE_average;
                this->i_dE_squared_average[itri] += _i_dE_squared_average;
                this->i_marker_den[itri] += _i_marker_den;

                this->e_dr_avg[itri] += _e_dr_average;
                this->e_dr_squared_average[itri] += _e_dr_squared_average;
                this->e_dE_avg[itri] += _e_dE_average;
                this->e_dE_squared_average[itri] += _e_dE_squared_average;
                this->e_marker_den[itri] += _e_marker_den;
            }
        }
        this->reader.EndStep();
        this->dup_writer.EndStep();
    }
    TIMER_STOP("ADIOS_STEP");

    TIMER_START("DATA_REDUCE");
    if (status == adios2::StepStatus::OK)
    {
        LOG << boost::format("Step %d: MPI reducing table vs mesh: %d %d") % this->istep % (total_nrow * NCOL) %
                   (this->ntriangle * 10);

        // Merge all to rank 0
        vec_reduce(this->i_dr_avg);
        vec_reduce(this->i_dr_squared_average);
        vec_reduce(this->i_dE_avg);
        vec_reduce(this->i_dE_squared_average);
        vec_reduce(this->i_marker_den);
        vec_reduce(this->e_dr_avg);
        vec_reduce(this->e_dr_squared_average);
        vec_reduce(this->e_dE_avg);
        vec_reduce(this->e_dE_squared_average);
        vec_reduce(this->e_marker_den);

        // Save
        if (this->rank == 0)
        {
            this->output();
        }

        this->istep++;
    }
    TIMER_STOP("DATA_REDUCE");

#ifdef CAM_TIMERS
    GPTLprint_memusage("STEP MEMUSAGE");
#endif
    TIMER_STOP("STEP");
    return status;
}

void Diffusion::output()
{
    TIMER_START("OUTPUT");
    static bool first = true;

    if (first)
    {
        this->output_io = ad->DeclareIO("diffusion");
        long unsigned int ntri = this->ntriangle;

        this->output_io.DefineVariable<double>("i_dr_avg", {ntri}, {0}, {ntri});
        this->output_io.DefineVariable<double>("i_dr_squared_average", {ntri}, {0}, {ntri});
        this->output_io.DefineVariable<double>("i_dE_avg", {ntri}, {0}, {ntri});
        this->output_io.DefineVariable<double>("i_dE_squared_average", {ntri}, {0}, {ntri});
        this->output_io.DefineVariable<double>("i_marker_den", {ntri}, {0}, {ntri});
        this->output_io.DefineVariable<double>("e_dr_avg", {ntri}, {0}, {ntri});
        this->output_io.DefineVariable<double>("e_dr_squared_average", {ntri}, {0}, {ntri});
        this->output_io.DefineVariable<double>("e_dE_avg", {ntri}, {0}, {ntri});
        this->output_io.DefineVariable<double>("e_dE_squared_average", {ntri}, {0}, {ntri});
        this->output_io.DefineVariable<double>("e_marker_den", {ntri}, {0}, {ntri});

        this->writer = output_io.Open("xgc.diffusion.bp", adios2::Mode::Write, MPI_COMM_SELF);

        first = false;
    }

    this->writer.BeginStep();
    this->writer.Put<double>("i_dr_avg", this->i_dr_avg.data());
    this->writer.Put<double>("i_dr_squared_average", this->i_dr_squared_average.data());
    this->writer.Put<double>("i_dE_avg", this->i_dE_avg.data());
    this->writer.Put<double>("i_dE_squared_average", this->i_dE_squared_average.data());
    this->writer.Put<double>("i_marker_den", this->i_marker_den.data());
    this->writer.Put<double>("e_dr_avg", this->e_dr_avg.data());
    this->writer.Put<double>("e_dr_squared_average", this->e_dr_squared_average.data());
    this->writer.Put<double>("e_dE_avg", this->e_dE_avg.data());
    this->writer.Put<double>("e_dE_squared_average", this->e_dE_squared_average.data());
    this->writer.Put<double>("e_marker_den", this->e_marker_den.data());
    this->writer.EndStep();

    TIMER_STOP("OUTPUT");
}