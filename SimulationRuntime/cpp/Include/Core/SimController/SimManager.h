#pragma once
/** @addtogroup coreSimcontroller
 *
 *  @{
 */
#include <Core/SimController/Configuration.h>
#include <Core/SimController/Initialization.h>
#include <Core/SimController/FactoryExport.h>
#include <Core/Utils/extension/logger.hpp>

#ifdef RUNTIME_PROFILING
#include <Core/Utils/extension/measure_time.hpp>
#endif

class SimManager
{
public:
    SimManager(boost::shared_ptr<IMixedSystem> system, Configuration* _config);
    ~SimManager();
    void runSimulation();
    void stopSimulation();
    void initialize();

    // for real-time usage (VxWorks and BODAS)
    void runSingleStep();

private:
    void computeEndTimes(std::vector<std::pair<double,int> > &tStopsSub);
    void computeSampleCycles();

    void runSingleProcess();
    void writeProperties();

    boost::shared_ptr<IMixedSystem> _mixed_system;
    Configuration* _config;

    std::vector<std::vector<std::pair<double,int> > > _tStops;            ///< - Stopzeitpunkte aufgrund von Time-Events
    boost::shared_ptr<ISolver>                        _solver;            ///< - Solver
    int                                               _dimtimeevent,      ///< Temp - Timeevent-Dimensionen-Array
                                                      _dimZeroFunc;       ///< - Number of zero functions
    int*                                              _timeEventCounter;  ///< Temp - Timeevent-Counter-Array
    int                                               _cycleCounter,
                                                      _resetCycle;
    ISolver::SOLVERCALL                               _solverTask;        ///< Temporary - Beschreibt die Aufgabe mit der der Solver aufgerufen wird
    int                                               _dbgId;              ///< Output - DebugID
    bool                                              _continueSimulation,///< - Flag für Endlossimulation (wird gesetzt, wenn Solver zurückkehrt)
                                                      _writeFinalState;   ///< Temporary - Ist am Ende noch ein Time-Event???
    bool*                                             _events;            ///< - Vector (of dimension _dimZeroF) indicating which zero function caused an event
    double                                            _H,                 ///< Input, Output - Koppelschrittweite
                                                      _tStart,
                                                      _tEnd,
                                                      _lastCycleTime;
    boost::shared_ptr<Initialization>                 _initialization;


    boost::shared_ptr<ITime> _timeevent_system;
    boost::shared_ptr<IEvent> _event_system;
    boost::shared_ptr<IContinuous> _cont_system;
    boost::shared_ptr<IStepEvent> _step_event_system;

    int* _sampleCycles;

    #ifdef RUNTIME_PROFILING
    std::vector<MeasureTimeData*> *measureTimeFunctionsArray;
    MeasureTimeValues *runSimStartValues, *runSimEndValues, *initSimStartValues, *initSimEndValues;
    #endif
};
/** @} */ // end of coreSimcontroller
