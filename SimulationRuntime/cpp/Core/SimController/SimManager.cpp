/** @addtogroup coreSimcontroller
 *
 *  @{
 */
#include <Core/ModelicaDefine.h>
#include <Core/Modelica.h>
#include <Core/SimController/SimManager.h>

#include <sstream>

SimManager::SimManager(boost::shared_ptr<IMixedSystem> system, Configuration* config)
  : _mixed_system      (system)
  , _config            (config)
  , _timeEventCounter  (NULL)
  , _events            (NULL)
  , _sampleCycles      (NULL)
  , _cycleCounter      (0)
  , _resetCycle        (0)
  , _lastCycleTime     (0)
  , _dbgId             (0)
{
    _solver = _config->createSelectedSolver(system.get());
    _initialization = boost::shared_ptr<Initialization>(new Initialization(boost::dynamic_pointer_cast<ISystemInitialization>(_mixed_system), _solver));

    #ifdef RUNTIME_PROFILING
    if(MeasureTime::getInstance() != NULL)
    {
        measureTimeFunctionsArray = new std::vector<MeasureTimeData*>(2, NULL); //0 runSimulation, initializeSimulation
        (*measureTimeFunctionsArray)[0] = new MeasureTimeData("initializeSimulation");
        (*measureTimeFunctionsArray)[1] = new MeasureTimeData("runSimulation");

        MeasureTime::addResultContentBlock(system->getModelName(),"simmanager",measureTimeFunctionsArray);

        initSimStartValues = MeasureTime::getZeroValues();
        initSimEndValues = MeasureTime::getZeroValues();
        runSimStartValues = MeasureTime::getZeroValues();
        runSimEndValues = MeasureTime::getZeroValues();
    }
    else
    {
        measureTimeFunctionsArray = new std::vector<MeasureTimeData*>();
        initSimStartValues = NULL;
        initSimEndValues = NULL;
        runSimStartValues = NULL;
        runSimEndValues = NULL;
    }
    #endif
}

SimManager::~SimManager()
{
    if (_timeEventCounter)
        delete[] _timeEventCounter;
    if (_events)
        delete[] _events;
    if (_sampleCycles)
        delete[] _sampleCycles;

    #ifdef RUNTIME_PROFILING
    if(initSimStartValues)
        delete initSimStartValues;
    if(initSimEndValues)
        delete initSimEndValues;
    if(runSimStartValues)
        delete runSimStartValues;
    if(runSimEndValues)
        delete runSimEndValues;
    #endif
}

void SimManager::initialize()
{
    #ifdef RUNTIME_PROFILING
    MEASURETIME_REGION_DEFINE(initSimHandler, "initializeSimulation");
    if (MeasureTime::getInstance() != NULL)
    {
        MEASURETIME_START(initSimStartValues, initSimHandler, "initializeSimulation");
    }
    #endif

    _cont_system = boost::dynamic_pointer_cast<IContinuous>(_mixed_system);
    _timeevent_system = boost::dynamic_pointer_cast<ITime>(_mixed_system);
    _event_system = boost::dynamic_pointer_cast<IEvent>(_mixed_system);
    _step_event_system = boost::dynamic_pointer_cast<IStepEvent>(_mixed_system);

    // Check dynamic casts
    if (!_event_system)
    {
    	throw ModelicaSimulationError(SIMMANAGER,"Could not get event system.");
    }
    if (!_cont_system)
    {
    	throw ModelicaSimulationError(SIMMANAGER,"Could not get continuous-event system.");
    }
    if (!_timeevent_system)
    {
    	throw ModelicaSimulationError(SIMMANAGER,"Could not get time-event system.");
    }
    if (!_step_event_system)
    {
    	throw ModelicaSimulationError(SIMMANAGER,"Could not get step-event system.");
    }

    Logger::write("SimManager: Start initialization",LC_INIT,LL_DEBUG);
    // Set flag for endless simulation (if solver returns)
    _continueSimulation = true;

    // Reset debug ID
    _dbgId = 0;

    try
    {
        // Build up system and update once
        _initialization->initializeSystem();
    }
    catch (std::exception& ex)
    {
        //ex << error_id(SIMMANAGER);
    	throw ModelicaSimulationError(SIMMANAGER,"Could not initialize system.",string(ex.what()),false);
    }

    if (_timeevent_system)
    {
        _dimtimeevent = _timeevent_system->getDimTimeEvent();
        if (_timeEventCounter)
            delete[] _timeEventCounter;
        _timeEventCounter = new int[_dimtimeevent];
        memset(_timeEventCounter, 0, _dimtimeevent * sizeof(int));
        // compute sampleCycles for RT simulation
        if (_config->getGlobalSettings()->useEndlessSim())
        {
            if (_sampleCycles)
                delete[] _sampleCycles;
            _sampleCycles = new int[_dimtimeevent];
            computeSampleCycles();
        }
    }
    else
        _dimtimeevent = 0;

    _tStart = _config->getGlobalSettings()->getStartTime();
    _tEnd = _config->getGlobalSettings()->getEndTime();
    // _solver->setTimeOut(_config->getGlobalSettings()->getAlarmTime());
    _dimZeroFunc = _event_system->getDimZeroFunc();
    _solverTask = ISolver::SOLVERCALL(ISolver::FIRST_CALL);
    if (_dimZeroFunc == _event_system->getDimZeroFunc())
    {
        if (_events)
            delete[] _events;
        _events = new bool[_dimZeroFunc];
        memset(_events, false, _dimZeroFunc * sizeof(bool));
    }

    Logger::write("SimManager: Assemble completed",LC_INIT,LL_DEBUG);
//#if defined(__TRICORE__) || defined(__vxworks)
    // Initialization for RT simulation
    if (_config->getGlobalSettings()->useEndlessSim())
    {
        _cycleCounter = 0;
        _resetCycle = _sampleCycles[0];
        for (int i = 1; i < _dimtimeevent; i++)
            _resetCycle *= _sampleCycles[i];
        // All Events are updated every cycle. In order to have a change in timeEventCounter, the reset is set to two
        if(_resetCycle == 1)
        	_resetCycle++;
        _solver->initialize();
    }
//#endif
    #ifdef RUNTIME_PROFILING
    if (MeasureTime::getInstance() != NULL)
    {
        MEASURETIME_END(initSimStartValues, initSimEndValues, (*measureTimeFunctionsArray)[0], initSimHandler);
    }
    #endif
}

void SimManager::runSingleStep()
{
    // Increase time event counter
	double cycletime = _config->getGlobalSettings()->gethOutput();
    if (_dimtimeevent && cycletime > 0.0)
    {

    	if (_lastCycleTime && cycletime != _lastCycleTime)
            throw ModelicaSimulationError(SIMMANAGER,"Cycle time can not be changed, if time events (samples) are present!");
        else
            _lastCycleTime = cycletime;

        for (int i = 0; i < _dimtimeevent; i++)
        {
            if (_cycleCounter % _sampleCycles[i] == 0)
                _timeEventCounter[i]++;
        }

        // Handle time event
        _timeevent_system->handleTimeEvent(_timeEventCounter);
        _cont_system->evaluateAll(IContinuous::CONTINUOUS);
        _event_system->saveAll();
        _timeevent_system->handleTimeEvent(_timeEventCounter);
    }
    // Solve
    _solver->solve(_solverTask);

    _cycleCounter++;
    // Reset everything to prevent overflows
    if (_cycleCounter == _resetCycle + 1)
    {
    	_cycleCounter = 1;
        for (int i = 0; i < _dimtimeevent; i++)
        	_timeEventCounter[i] = 0;
    }
}

void SimManager::computeSampleCycles()
{
    int counter = 0;
    time_event_type timeEventPairs;                        ///< - Contains start times and time spans

    _timeevent_system->getTimeEvent(timeEventPairs);
    std::vector<std::pair<double, double> >::iterator iter;
    iter = timeEventPairs.begin();
    for (; iter != timeEventPairs.end(); ++iter)
    {
        if (iter->first != 0.0 || iter->second == 0.0)
        {
            throw ModelicaSimulationError(SIMMANAGER,"Time event not starting at t=0.0 or not cyclic!");
        }
        else
        {
            // Check if sample time is a multiple of the cycle time (with a tolerance)
            if ((iter->second / _config->getGlobalSettings()->gethOutput()) - int((iter->second / _config->getGlobalSettings()->gethOutput()) + 0.5) <= 1e6 * UROUND)
            {
                _sampleCycles[counter] = int((iter->second / _config->getGlobalSettings()->gethOutput()) + 0.5);
            }
            else
            {
                throw ModelicaSimulationError(SIMMANAGER,"Sample time is not a multiple of the cycle time!");
            }

        }
        counter++;
    }
}

void SimManager::runSimulation()
{
    #ifdef RUNTIME_PROFILING
    MEASURETIME_REGION_DEFINE(runSimHandler, "runSimulation");
    if (MeasureTime::getInstance() != NULL)
    {
        MEASURETIME_START(runSimStartValues, runSimHandler, "runSimulation");
    }
    #endif
    try
    {
        Logger::write("SimManager: Start simulation at t = " + boost::lexical_cast<std::string>(_tStart),LC_SOLV,LL_INFO);
        runSingleProcess();
        // Measure time; Output SimInfos
        ISolver::SOLVERSTATUS status = _solver->getSolverStatus();
        if ((status & ISolver::DONE) || (status & ISolver::USER_STOP))
        {
            //Logger::write("SimManager: Simulation done at t = " + boost::lexical_cast<std::string>(_tEnd),LC_SOLV,LL_INFO);
            writeProperties();
        }
    }
    catch (std::exception & ex)
    {
        Logger::write("SimManager: Simulation finish with errors at t = " + boost::lexical_cast<std::string>(_tEnd),LC_SOLV,LL_ERROR);
        writeProperties();

        Logger::write("SimManager: Error = " + boost::lexical_cast<std::string>(ex.what()),LC_SOLV,LL_ERROR);
        //ex << error_id(SIMMANAGER);
        throw;
    }
    #ifdef RUNTIME_PROFILING
    if (MeasureTime::getInstance() != NULL)
    {
        MEASURETIME_END(runSimStartValues, runSimEndValues, (*measureTimeFunctionsArray)[1], runSimHandler);
    }
    #endif
}

void SimManager::stopSimulation()
{
    if (_solver)
        _solver->stop();
}

void SimManager::writeProperties()
{
	// declaration for Logging
	std::pair<LogCategory, LogLevel> logM = Logger::getLogMode(LC_SOLV, LL_INFO);

    Logger::write(boost::lexical_cast<std::string>("SimManager: Computation time"),logM);
    Logger::write(boost::lexical_cast<std::string>("SimManager: Simulation end time:          ") + boost::lexical_cast<std::string>(_tEnd),logM);
    //Logger::write(boost::lexical_cast<std::string>("Rechenzeit in Sekunden:                 ") + boost::lexical_cast<std::string>(_tClockEnd-_tClockStart),logM);

    Logger::write(boost::lexical_cast<std::string>("Simulation info from solver:"),logM);
    _solver->writeSimulationInfo();
/*
     // Zeit
    if(_settings->_globalSettings->bEndlessSim)
    {
		 Logger::write(boost::lexical_cast<std::string>("Geforderte Simulationszeit: endlos"),logM);
		 //Logger::write(boost::lexical_cast<std::string>("Rechenzeit:                 ") + boost::lexical_cast<std::string>(_tClockEnd-_tClockStart),logM);
		 Logger::write(boost::lexical_cast<std::string>("Endzeit Toleranz:           ") + boost::lexical_cast<std::string>(config->getSimControllerSettings()->dTendTol),logM);
	}
    else
    {
    	Logger::write(boost::lexical_cast<std::string>("Geforderte Simulationszeit: ") + boost::lexical_cast<std::string>(_tEnd),logM);
    	//_infoStream << "Rechenzeit:                 " << (_tClockEnd-_tClockStart);
    	//Logger::write(boost::lexical_cast<std::string>("Rechenzeit:                 ") + boost::lexical_cast<std::string>(_tClockEnd-_tClockStart),logM);
     	Logger::write(boost::lexical_cast<std::string>("Endzeit Toleranz:           ") + boost::lexical_cast<std::string>(_config->getSimControllerSettings()->dTendTol),logM);
     }

     if(_settings->_globalSettings->bRealtimeSim)
     {
    	 Logger::write(boost::lexical_cast<std::string>("Echtzeit Simulationszeit aktiv:"),logM);
    	 log->wirte(boost::lexical_cast<std::string>("Faktor:                 ") + boost::lexical_cast<std::string>(_settings->_globalSettings->dRealtimeFactor),logM);
    	 Logger::write(boost::lexical_cast<std::string>("Aktive Rechenzeit (Pause Zeit):           ") + boost::lexical_cast<std::string>(_tClockEnd-_tClockStart-_dataPool->getPauseDelay())
    			  + boost::lexical_cast<std::string>("(") + boost::lexical_cast<std::string>(_dataPool->getPauseDelay()) + boost::lexical_cast<std::string>(")"),logM);
     }
     if(_dimSolver == 1)
     {
    	 if(!(_solver->getSolverStatus() & ISolver::ERROR_STOP))
    		 Logger::write(boost::lexical_cast<std::string>("Simulation erfolgreich."),logM);
    	 else
    		 Logger::write(boost::lexical_cast<std::string>("Fehler bei der Simulation!"),logM);

    	 Logger::write(boost::lexical_cast<std::string>("Schritte insgesamt des Solvers:   ") + boost::lexical_cast<std::string>(_totStps.at(0)),logM);
    	 Logger::write(boost::lexical_cast<std::string>("Akzeptierte Schritte des Solvers: ") + boost::lexical_cast<std::string>(_accStps.at(0)),logM);
    	 log->wrtie(boost::lexical_cast<std::string>("Verworfene Schritte  des Solvers: ") + boost::lexical_cast<std::string>(_rejStps.at(0)),logM);

    	 if(Logger::getInstance()->isOutput(logM)
    		_solver->writeSimulationInfo();

     }
     else
     {
		 Logger::write(boost::lexical_cast<std::string>("Anzahl Solver: ") + boost::lexical_cast<std::string>(_dimSolver),logM) ;

		 if(_completelyDecoupledSystems || !(_settings->bDynCouplingStepSize))
		 {
			Logger::write(boost::lexical_cast<std::string>("Koppelschrittweitensteuerung: fix "),logM);
			Logger::write(boost::lexical_cast<std::string>("Ausgabeschrittweite:          ") + boost::lexical_cast<std::string>(_config->getGlobalSettings()->gethOutput()),logM);
			Logger::write(boost::lexical_cast<std::string>("Koppelschrittweite:           ") + boost::lexical_cast<std::string>(_settings->dHcpl), logM);

			Logger::write(boost::lexical_cast<std::string>("Anzahl Koppelschritte: ") + boost::lexical_cast<std::string>(_totCouplStps),logM) ;

			if(abs(_settings->_globalSettings->tEnd - _tEnd) < 10*UROUND)
				Logger::write(boost::lexical_cast<std::string>("Integration erfolgreich. IDID= ") + boost::lexical_cast<std::string>(_dbgId), logM);
			else
				Logger::write(boost::lexical_cast<std::string>("Solver run time simmgr_error. "),logM);

		 }
		 else
		 {
			 Logger::write(boost::lexical_cast<std::string>("Koppelschrittweitensteuerung:            dynamisch"),logM);
			 Logger::write(boost::lexical_cast<std::string>("Ausgabeschrittweite:                     ") + boost::lexical_cast<std::string>(_config->getGlobalSettings()->gethOutput()),logM);

			 Logger::write(boost::lexical_cast<std::string>("Koppelschrittweite für nächsten Schritt: ") + boost::lexical_cast<std::string>(_H),logM);
			 Logger::write(boost::lexical_cast<std::string>("Maximal verwendete Schrittweite:         ") + boost::lexical_cast<std::string>(_Hmax),logM) ;
			 Logger::write(boost::lexical_cast<std::string>("Minimal verwendete Schrittweite:         ") + boost::lexical_cast<std::string>(_Hmin),logM) ;

			 Logger::write(boost::lexical_cast<std::string>("Obere Grenze für Schrittweite:           ") + boost::lexical_cast<std::string>(_settings->dHuplim),logM);
			 Logger::write(boost::lexical_cast<std::string>("Untere Grenze für Schrittweite:          ") + boost::lexical_cast<std::string>(_settings->dHlowlim),logM);
			 Logger::write(boost::lexical_cast<std::string>("k-Faktor für Schrittweite:               ") + boost::lexical_cast<std::string>(_settings->dK),logM);
			 Logger::write(boost::lexical_cast<std::string>("Savety-Faktor:                           ") + boost::lexical_cast<std::string>(_settings->dC),logM);
			 Logger::write(boost::lexical_cast<std::string>("Upscale-Faktor:                          ") + boost::lexical_cast<std::string>(_settings->dCmax),logM);
			 Logger::write(boost::lexical_cast<std::string>("Downscale-Faktor:                        ") + boost::lexical_cast<std::string>(_settings->dCmin),logM);
			 Logger::write(boost::lexical_cast<std::string>("Fehlertoleranz:                          ") + boost::lexical_cast<std::string>(_settings->dErrTol),logM);
			 Logger::write(boost::lexical_cast<std::string>("Fehlertoleranz für Single Step:          ") + boost::lexical_cast<std::string>(_settings->dSingleStepTol),logM);

			 Logger::write(boost::lexical_cast<std::string>("Anzahl Koppelschritte insgesamt:         ") + boost::lexical_cast<std::string>(_totCouplStps),logM);
			 Logger::write(boost::lexical_cast<std::string>("Anzahl Einfach-Schritte:                 ") + boost::lexical_cast<std::string>(_singleStps),logM);
			 Logger::write(boost::lexical_cast<std::string>("Davon akzeptierte Schritte:              ") + boost::lexical_cast<std::string>(_accCouplStps),logM);
			 Logger::write(boost::lexical_cast<std::string>("Davon verworfene Schritte:               ") + boost::lexical_cast<std::string>(_rejCouplStps),logM);

			 Logger::write(boost::lexical_cast<std::string>("Max. nacheinander verwerfbare Schritte:  ") + boost::lexical_cast<std::string>(_settings->iMaxRejSteps),logM);
			 Logger::write(boost::lexical_cast<std::string>("Max. nacheinander verworfene Schritte:   ") + boost::lexical_cast<std::string>(_rejCouplStpsRow),logM);
			 Logger::write(boost::lexical_cast<std::string>("Zeitpunkt meiste verworfene Schritte:    ") + boost::lexical_cast<std::string>(_tRejCouplStpsRow),logM);

			 Logger::write(boost::lexical_cast<std::string>("Anfangsschrittweite:                     ") + boost::lexical_cast<std::string>(_Hinit),logM);
			 Logger::write(boost::lexical_cast<std::string>("bei einem Fehler:                        ") + boost::lexical_cast<std::string>(_simmgr_errorInit),logM);
			 Logger::write(boost::lexical_cast<std::string>("nach verworfenen Schritten:              ") + boost::lexical_cast<std::string>(_rejCouplStpsInit),logM);

			 Logger::write(boost::lexical_cast<std::string>("Wenn Fehler knapp unter 1+ErrTol bei 0 verworf. Schritte, dann war Anfangsschrittweite gut gewählt.\n\n"),logM);

			 if(_dbgId == 0 && (abs(_settings->_globalSettings->tEnd - _tEnd) < 10*UROUND))
				 Logger::write(boost::lexical_cast<std::string>("Integration erfolgreich. IDID= ") + boost::lexical_cast<std::string>(_dbgId) + boost::lexical_cast<std::string>("\n\n"),logM);
			 else if(_dbgId == -1)
				 Logger::write(boost::lexical_cast<std::string>("Integration abgebrochen. Fehlerbetrag zu groß (ev. Kopplung zu starr?). IDID= ") + boost::lexical_cast<std::string>(_dbgId),logM);
			 else if(_dbgId == -2)
				 Logger::write(boost::lexical_cast<std::string>("Integration abgebrochen. Mehr als ") + boost::lexical_cast<std::string>(_settings->iMaxRejSteps)
						  + boost::lexical_cast<std::string>(" dirket nacheinander verworfenen Schritte. IDID= ") + boost::lexical_cast<std::string>(_dbgId),logM);
			 else if(_dbgId == -3)
				 Logger::write(boost::lexical_cast<std::string>("Integration abgebrochen. Koppelschrittweite kleiner als ") + boost::lexical_cast<std::string>(_settings->dHlowlim)
						 	+ boost::lexical_cast<std::string>(". IDID= ") + boost::lexical_cast<std::string>(_dbgId),logM);
			 else
				 Logger::write(boost::lexical_cast<std::string>("Solver run time simmgr_error"),logM);

		 }

		 // Schritte der Solver
		 for(int i=0; i<_dimSolver; ++i)
		 {
			 if(!(_solver[i]->getSolverStatus() & ISolver::ERROR_STOP))
				 Logger::write(boost::lexical_cast<std::string>("Simulation mit Solver[") + boost::lexical_cast<std::string>(i) + boost::lexical_cast<std::string>("] erfolgreich."),logM);
			 else
				 Logger::write(boost::lexical_cast<std::string>("Fehler bei der Simulation in Solver[") + boost::lexical_cast<std::string>(i) + boost::lexical_cast<std::string>("]!"),logM);

			 Logger::write(boost::lexical_cast<std::string>("Schritte insgesamt Solver[") + boost::lexical_cast<std::string>(i) + boost::lexical_cast<std::string>("]:   ") + boost::lexical_cast<std::string>(_totStps.at(i)),logM);
			 Logger::write(boost::lexical_cast<std::string>("Akzeptierte Schritte Solver[") + boost::lexical_cast<std::string>(i) + boost::lexical_cast<std::string>("]: ") + boost::lexical_cast<std::string>(_accStps.at(i)),logM);
			 Logger::write(boost::lexical_cast<std::string>("Verworfene Schritte  Solver[") + boost::lexical_cast<std::string>(i) + boost::lexical_cast<std::string>("]: ") + boost::lexical_cast<std::string>(_rejStps.at(i)),logM);

		 }

			 // Solver-Properties
		 for(int i=0; i<_dimSolver; ++i)
		 {
			 Logger::write(boost::lexical_cast<std::string>("-----------------------------------------"),logM);
			 Logger::write(boost::lexical_cast<std::string>("simmgr_info Ausgabe Solver[") + boost::lexical_cast<std::string>(i) + boost::lexical_cast<std::string>("]"),logM);

			 if(Logger::getInstance()->isOutput(logM))
				 _solver[i]->writeSimulationInfo(os);
		 }
     }

     */
}

void SimManager::computeEndTimes(std::vector<std::pair<double, int> > &tStopsSub)
{
    int counterTimes = 0, counterEvents = 0;
    time_event_type timeEventPairs;                        ///< - Beinhaltet Frequenzen und Startzeit der Time-Events
    _writeFinalState = true;

    if (tStopsSub.size() == 0)
    {
        _timeevent_system->getTimeEvent(timeEventPairs);
        std::vector<std::pair<double, double> >::iterator iter;
        iter = timeEventPairs.begin();
        for (; iter != timeEventPairs.end(); ++iter)
        {
            if (iter->second != 0)
            {
                counterTimes = 0;
                if (iter->first <= UROUND)
                {
                    _timeEventCounter[counterEvents]++;
                    counterTimes++;
                    _solverTask = ISolver::SOLVERCALL(_solverTask | ISolver::RECALL);
                }
                while (iter->first + counterTimes * (iter->second) < _tEnd)
                {
                    tStopsSub.push_back(std::make_pair(iter->first + counterTimes * (iter->second), counterEvents));
                    counterTimes++;
                }
            }
            else
            {
                if (iter->first <= UROUND)
                {
                    _timeEventCounter[counterEvents]++;
                    counterTimes++;
                    _solverTask = ISolver::SOLVERCALL(_solverTask | ISolver::RECALL);
                }
                else
                {
                    if (iter->first <= _tEnd)
                        tStopsSub.push_back(std::make_pair(iter->first, counterEvents));
                }
            }
            counterEvents++;
        }  // end for iter tStops
        sort(tStopsSub.begin(), tStopsSub.end());
        if (tStopsSub.size() == 0)
        {
            tStopsSub.push_back(std::make_pair(_tEnd, 0));
            _writeFinalState = false;
        }
    }  // end if endlessSim
    else
    {
        tStopsSub.erase(tStopsSub.begin(), tStopsSub.end());
        std::vector<std::pair<double, double> >::iterator iter;
        iter = timeEventPairs.begin();
        for (; iter != timeEventPairs.end(); ++iter)
        {
            if (iter->second != 0)
            {
                counterTimes = 1;
                if (abs(iter->first) <= UROUND)
                {
                    counterTimes++;
                    _solverTask = ISolver::SOLVERCALL(_solverTask | ISolver::RECALL);
                }
                while (_tStart + iter->first + counterTimes * (iter->second) < _tEnd)
                {
                    tStopsSub.push_back(std::make_pair(_tStart + iter->first + counterTimes * (iter->second), counterEvents));
                    counterTimes++;
                }
            }
            else
            {
                if (iter->first < _tStart)
                {
                    continue;
                }
                else
                {
                    if (iter->first <= _tEnd)
                        tStopsSub.push_back(std::make_pair(iter->first, counterEvents));
                    if (abs(iter->first) <= UROUND)
                        _solverTask = ISolver::SOLVERCALL(_solverTask | ISolver::RECALL);
                }
            }
            counterEvents++;
        }  // end for iter tStops
        sort(tStopsSub.begin(), tStopsSub.end());
        if (tStopsSub.size() == 0)
        {
            tStopsSub.push_back(std::make_pair(_tEnd, 0));
            _writeFinalState = false;
        }
    }
}

void SimManager::runSingleProcess()
{
    double startTime, endTime, *zeroVal_0, *zeroVal_new;
    int dimZeroF;

    std::vector<std::pair<double, int> > tStopsSub;

    _H = _tEnd;
    _solverTask = ISolver::SOLVERCALL(_solverTask | ISolver::RECORDCALL);
    _solver->setStartTime(_tStart);
    _solver->setEndTime(_tEnd);

    _solver->solve(_solverTask);
    _solverTask = ISolver::SOLVERCALL(_solverTask ^ ISolver::RECORDCALL);
    /* Logs temporarily disabled
     BOOST_LOG_SEV(simmgr_lg::get(), simmgr_normal) <<"Run single process." ; */
    Logger::write("SimManager: Run single process",LC_SOLV,LL_DEBUG);

    memset(_timeEventCounter, 0, _dimtimeevent * sizeof(int));
    computeEndTimes(tStopsSub);
    _tStops.push_back(tStopsSub);
    dimZeroF = _event_system->getDimZeroFunc();
    zeroVal_new = new double[dimZeroF];
    _timeevent_system->setTime(_tStart);
    if (_dimtimeevent)
    {
        _timeevent_system->handleTimeEvent(_timeEventCounter);
    }
    _cont_system->evaluateAll(IContinuous::CONTINUOUS);      // vxworksupdate
    _event_system->getZeroFunc(zeroVal_new);

    for (int i = 0; i < _dimZeroFunc; i++)
        _events[i] = bool(zeroVal_new[i]);
    _mixed_system->handleSystemEvents(_events);
    //_cont_system->evaluateODE(IContinuous::CONTINUOUS);
    // Reset the time-events
    if (_dimtimeevent)
    {
        _timeevent_system->handleTimeEvent(_timeEventCounter);
    }

    std::vector<std::pair<double, int> >::iterator iter;
    iter = _tStops[0].begin();
    /* time measurement temporary disabled
     // Startzeit messen
     _tClockStart = Time::Time().getSeconds();
     */
    startTime = _tStart;
    bool user_stop = false;

    while (_continueSimulation)
    {
        for (; iter != _tStops[0].end(); ++iter)
        {
            endTime = iter->first;

            // Set start time, end time, initial step size
            _solver->setStartTime(startTime);
            _solver->setEndTime(endTime);
            _solver->setInitStepSize(_config->getGlobalSettings()->gethOutput());
            _solver->solve(_solverTask);

            if (_solverTask & ISolver::FIRST_CALL)
            {
                _solverTask = ISolver::SOLVERCALL(_solverTask ^ ISolver::FIRST_CALL);
                _solverTask = ISolver::SOLVERCALL(_solverTask | ISolver::RECALL);
            }
            startTime = endTime;
            if (_dimtimeevent)
            {
              // Find all time events at the current time
              while((iter !=_tStops[0].end()) && (abs(iter->first - endTime) <1e4*UROUND))
              {
                _timeEventCounter[iter->second]++;
                iter++;
              }
              // Set the iterator back to the current end time
              iter--;

                    // Then handle time events
                    _timeevent_system->handleTimeEvent(_timeEventCounter);

                    _event_system->getZeroFunc(zeroVal_new);
                    for (int i = 0; i < _dimZeroFunc; i++)
                      _events[i] = bool(zeroVal_new[i]);
                    _mixed_system->handleSystemEvents(_events);
                    // Reset time-events
                    _timeevent_system->handleTimeEvent(_timeEventCounter);
            }

            user_stop = (_solver->getSolverStatus() & ISolver::USER_STOP);
            if (user_stop)
              break;
        }  // end for time events

        if (abs(_tEnd - endTime) > _config->getSimControllerSettings()->dTendTol && !user_stop)
        {
            startTime = endTime;
            _solver->setStartTime(startTime);
            _solver->setEndTime(_tEnd);
            _solver->setInitStepSize(_config->getGlobalSettings()->gethOutput());
            _solver->solve(_solverTask);
            // In _solverTask FIRST_CALL Bit löschen und RECALL Bit setzen
            if (_solverTask & ISolver::FIRST_CALL)
            {
                _solverTask = ISolver::SOLVERCALL(_solverTask ^ ISolver::FIRST_CALL);
                _solverTask = ISolver::SOLVERCALL(_solverTask | ISolver::RECALL);
            }
            if (user_stop)
                break;
        }  // end if weiter nach Time Events
        else  // Event am Schluss recorden.
        {
            if (_writeFinalState)
            {
                _solverTask = ISolver::SOLVERCALL(ISolver::RECORDCALL);
                _solver->solve(_solverTask);
            }
        }

        // Beendigung der Simulation
        if ((!(_config->getGlobalSettings()->useEndlessSim())) || (_solver->getSolverStatus() & ISolver::SOLVERERROR) || (_solver->getSolverStatus() & ISolver::USER_STOP))
        {
            _continueSimulation = false;
        }

        // Endless simulation
        else
        {
            // Zeitinvervall hochzählen
            _tStart = _tEnd;
            _tEnd += _H;

            computeEndTimes(tStopsSub);
            _tStops.push_back(tStopsSub);
            if (_dimtimeevent)
            {
                if (zeroVal_new)
                {
                    _timeevent_system->handleTimeEvent(_timeEventCounter);
                    _cont_system->evaluateAll(IContinuous::CONTINUOUS);   // vxworksupdate
                    _event_system->getZeroFunc(zeroVal_new);
                    for (int i = 0; i < _dimZeroFunc; i++)
                        _events[i] = bool(zeroVal_new[i]);
                    _mixed_system->handleSystemEvents(_events);
                    //_cont_system->evaluateODE(IContinuous::CONTINUOUS);
                    //reset time-events
                    _timeevent_system->handleTimeEvent(_timeEventCounter);
                }
            }

            iter = _tStops[0].begin();
        }

    }  // end while continue
    _step_event_system->setTerminal(true);
    _cont_system->evaluateAll(IContinuous::CONTINUOUS); //Is this really necessary? The solver should have already calculated the "final time point"

    if (zeroVal_new)
        delete[] zeroVal_new;

}  // end singleprocess
/** @} */ // end of coreSimcontroller
