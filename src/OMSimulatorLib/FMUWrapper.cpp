/*
 * This file is part of OpenModelica.
 *
 * Copyright (c) 1998-CurrentYear, Open Source Modelica Consortium (OSMC),
 * c/o Linköpings universitet, Department of Computer and Information Science,
 * SE-58183 Linköping, Sweden.
 *
 * All rights reserved.
 *
 * THIS PROGRAM IS PROVIDED UNDER THE TERMS OF GPL VERSION 3 LICENSE OR
 * THIS OSMC PUBLIC LICENSE (OSMC-PL) VERSION 1.2.
 * ANY USE, REPRODUCTION OR DISTRIBUTION OF THIS PROGRAM CONSTITUTES
 * RECIPIENT'S ACCEPTANCE OF THE OSMC PUBLIC LICENSE OR THE GPL VERSION 3,
 * ACCORDING TO RECIPIENTS CHOICE.
 *
 * The OpenModelica software and the Open Source Modelica
 * Consortium (OSMC) Public License (OSMC-PL) are obtained
 * from OSMC, either from the above address,
 * from the URLs: http://www.ida.liu.se/projects/OpenModelica or
 * http://www.openmodelica.org, and in the OpenModelica distribution.
 * GNU version 3 is obtained from: http://www.gnu.org/copyleft/gpl.html.
 *
 * This program is distributed WITHOUT ANY WARRANTY; without
 * even the implied warranty of  MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE, EXCEPT AS EXPRESSLY SET FORTH
 * IN THE BY RECIPIENT SELECTED SUBSIDIARY LICENSE CONDITIONS OF OSMC-PL.
 *
 * See the full OSMC Public License conditions for more details.
 *
 */

#include "FMUWrapper.h"
#include "Variable.h"
#include "DirectedGraph.h"
#include "Logging.h"
#include "Resultfile.h"
#include "Settings.h"
#include "GlobalSettings.h"
#include "CompositeModel.h"
#include "Util.h"

#include <fmilib.h>
#include <JM/jm_portability.h>

#include <iostream>
#include <string>
#include <map>
#include <stdlib.h>

#include <boost/filesystem.hpp>

#include "cvode/cvode.h"             /* prototypes for CVODE fcts., consts. */
#include "nvector/nvector_serial.h"  /* serial N_Vector types, fcts., macros */
#include "cvode/cvode_dense.h"       /* prototype for CVDense */
#include "sundials/sundials_dense.h" /* definitions DlsMat DENSE_ELEM */
#include "sundials/sundials_types.h" /* definition of type realtype */

void fmiLogger(jm_callbacks* c, jm_string module, jm_log_level_enu_t log_level, jm_string message)
{
  switch (log_level)
  {
  case jm_log_level_info:
    logDebug("module " + std::string(module) + ": " + std::string(message));
    break;
  case jm_log_level_warning:
    logWarning("module " + std::string(module) + ": " + std::string(message));
    break;
  case jm_log_level_error:
    logError("module " + std::string(module) + ": " + std::string(message));
    break;
  case jm_log_level_fatal:
    logFatal("module " + std::string(module) + ": " + std::string(message));
    break;
  default:
    logWarning("[log level " + std::string(jm_log_level_to_string(log_level)) + "] module " + std::string(module) + ": " + std::string(message));
  }
}

void fmi2logger(fmi2_component_environment_t env, fmi2_string_t instanceName, fmi2_status_t status, fmi2_string_t category, fmi2_string_t message, ...)
{
  int len;
  char msg[1000];
  va_list argp;
  va_start(argp, message);
  len = vsnprintf(msg, 1000, message, argp);

  switch (status)
  {
  case fmi2_status_ok:
  case fmi2_status_pending:
    logDebug(std::string(instanceName) + " (" + category + "): " + msg);
    break;
  case fmi2_status_warning:
    logWarning(std::string(instanceName) + " (" + category + "): " + msg);
    break;
  case fmi2_status_error:
  case fmi2_status_discard:
    logError(std::string(instanceName) + " (" + category + "): " + msg);
    break;
  case fmi2_status_fatal:
    logFatal(std::string(instanceName) + " (" + category + "): " + msg);
    break;
  default:
    logWarning("fmiStatus = " + std::string(fmi2_status_to_string(status)) + "; " + instanceName + " (" + category + "): " + msg);
  }
}

int cvode_rhs(realtype t, N_Vector y, N_Vector ydot, void *user_data)
{
  FMUWrapper *fmu = (FMUWrapper*)user_data;

  // update states in FMU
  for (size_t i = 0; i < fmu->n_states; ++i)
    fmu->states[i] = NV_Ith_S(y, i);

  // set states
  fmi2_status_t fmistatus;
  fmistatus = fmi2_import_set_continuous_states(fmu->fmu, fmu->states, fmu->n_states);
  if (fmi2_status_ok != fmistatus) logFatal("fmi2_import_set_continuous_states failed");
  // get state derivatives
  fmistatus = fmi2_import_get_derivatives(fmu->fmu, fmu->states_der, fmu->n_states);
  if (fmi2_status_ok != fmistatus) logFatal("fmi2_import_get_derivatives failed");

  for (size_t i = 0; i < fmu->n_states; ++i)
    NV_Ith_S(ydot, i) = fmu->states_der[i];

  return 0;
}

FMUWrapper::FMUWrapper(CompositeModel& model, std::string fmuPath, std::string instanceName)
  : model(model), fmuPath(fmuPath), instanceName(instanceName), solverMethod(EXPLICIT_EULER)
{
  logTrace();

  if (!boost::filesystem::exists(fmuPath))
    logFatal("Specified file name does not exist: \"" + fmuPath + "\"");

  callbacks.malloc = malloc;
  callbacks.calloc = calloc;
  callbacks.realloc = realloc;
  callbacks.free = free;
  callbacks.logger = fmiLogger;
#ifdef OMS_DEBUG_LOGGING
  callbacks.log_level = jm_log_level_all;
#else
  callbacks.log_level = jm_log_level_warning;
#endif
  callbacks.context = 0;

  //set working directory
  tempDir = fmi_import_mk_temp_dir(&callbacks, GlobalSettings::getInstance().GetTempDirectory().c_str(), "temp_");
  logDebug("set working directory to \"" + tempDir + "\"");

  context = fmi_import_allocate_context(&callbacks);

  // check version of FMU
  fmi_version_enu_t version = fmi_import_get_fmi_version(context, fmuPath.c_str(), tempDir.c_str());
  if (fmi_version_2_0_enu != version)
  {
    logError("Unsupported FMI version: " + std::string(fmi_version_to_string(version)));
    return;
  }

  // parse modelDescription.xml
  fmu = fmi2_import_parse_xml(context, tempDir.c_str(), 0);
  if (!fmu)
    logFatal("Error parsing modelDescription.xml");

  // check FMU kind (CS or ME)
  fmuKind = fmi2_import_get_fmu_kind(fmu);
  if (fmi2_fmu_kind_me == fmuKind)
    logDebug("FMU ME");
  else if (fmi2_fmu_kind_cs == fmuKind)
    logDebug("FMU CS");
  else if (fmi2_fmu_kind_me_and_cs == fmuKind)
    logDebug("FMU ME & CS");
  else
    logError("Unsupported FMU kind: " + std::string(fmi2_fmu_kind_to_string(fmuKind)));

  callBackFunctions.logger = fmi2logger;
  callBackFunctions.allocateMemory = calloc;
  callBackFunctions.freeMemory = free;
  callBackFunctions.componentEnvironment = fmu;
  callBackFunctions.stepFinished = NULL;

  if (fmi2_fmu_kind_me == fmuKind)
  {
    jm_status_enu_t jmstatus;

    //Load the FMU shared library
    jmstatus = fmi2_import_create_dllfmu(fmu, fmi2_fmu_kind_me, &callBackFunctions);
    if (jm_status_error == jmstatus) logFatal("Could not create the DLL loading mechanism (C-API). Error: " + std::string(fmi2_import_get_last_error(fmu)));

    logDebug("Version returned from FMU: " + std::string(fmi2_import_get_version(fmu)));
    logDebug("Platform type returned: " + std::string(fmi2_import_get_types_platform(fmu)));
    logDebug("GUID: " + std::string(fmi2_import_get_GUID(fmu)));

    fmi2_string_t instanceName = "ME-FMU instance";
    jmstatus = fmi2_import_instantiate(fmu, instanceName, fmi2_model_exchange, NULL, fmi2_false);
    if (jm_status_error == jmstatus) logFatal("fmi2_import_instantiate failed");
  }
  else if (fmi2_fmu_kind_cs == fmuKind || fmi2_fmu_kind_me_and_cs == fmuKind)
  {
    jm_status_enu_t jmstatus;

    //Load the FMU shared library
    jmstatus = fmi2_import_create_dllfmu(fmu, fmi2_fmu_kind_cs, &callBackFunctions);
    if (jm_status_error == jmstatus) logFatal("Could not create the DLL loading mechanism (C-API). Error: " + std::string(fmi2_import_get_last_error(fmu)));

    logDebug("Version returned from FMU: " + std::string(fmi2_import_get_version(fmu)));
    logDebug("Platform type returned: " + std::string(fmi2_import_get_types_platform(fmu)));
    logDebug("GUID: " + std::string(fmi2_import_get_GUID(fmu)));

    fmi2_string_t instanceName = "CS-FMU instance";
    jmstatus = fmi2_import_instantiate(fmu, instanceName, fmi2_cosimulation, NULL, fmi2_false);
    if (jm_status_error == jmstatus) logFatal("fmi2_import_instantiate failed");
  }

  // create variable list
  fmi2_import_variable_list_t *varList = fmi2_import_get_variable_list(fmu, 0);
  size_t varListSize = fmi2_import_get_variable_list_size(varList);
  logDebug(toString(varListSize) + " variables");
  for (size_t i = 0; i < varListSize; ++i)
  {
    fmi2_import_variable_t* var = fmi2_import_get_variable(varList, i);
    Variable v(var, this);
    allVariables.push_back(v);
  }
  fmi2_import_free_variable_list(varList);

  // mark states
  varList = fmi2_import_get_derivatives_list(fmu);
  varListSize = fmi2_import_get_variable_list_size(varList);
  logDebug(toString(varListSize) + " states");
  for (size_t i = 0; i < varListSize; ++i)
  {
    fmi2_import_variable_t* var = fmi2_import_get_variable(varList, i);
    fmi2_import_real_variable_t* varReal = fmi2_import_get_variable_as_real(var);
    fmi2_import_variable_t* varState = (fmi2_import_variable_t*)fmi2_import_get_real_variable_derivative_of(varReal);
    if (varState)
    {
      fmi2_value_reference_t state_vr = fmi2_import_get_variable_vr(varState);
      Variable* state_var = getVariable(state_vr);
      if (state_var)
        state_var->markAsState();
      else
        logError("Couldn't find " + toString(fmi2_import_get_variable_name(varState)));
    }
    else
      logError("Couldn't map " + toString(fmi2_import_get_variable_name(var)) + " to the corresponding state variable");
  }
  fmi2_import_free_variable_list(varList);

  // create some special variable maps
  for (int i = 0; i < allVariables.size(); i++)
  {
    if (allVariables[i].isInitialUnknown())
      initialUnknowns.push_back(i + 1);
    if (allVariables[i].isInput())
      allInputs.push_back(i + 1);
    if (allVariables[i].isOutput())
      allOutputs.push_back(i + 1);
    if (allVariables[i].isParameter())
      allParameters.push_back(i + 1);

    switch (allVariables[i].getBaseType())
    {
    case fmi2_base_type_real:
      realVariables.push_back(i + 1);
      break;
    case fmi2_base_type_int:
      intVariables.push_back(i + 1);
      break;
    case fmi2_base_type_bool:
      boolVariables.push_back(i + 1);
      break;
    case fmi2_base_type_str:
      strVariables.push_back(i + 1);
      break;
    case fmi2_base_type_enum:
      enumVariables.push_back(i + 1);
      break;
    default:
      logWarning("FMUWrapper: Unsupported base type");
      break;
    }
  }

  // generate internal dependency graphs
  getDependencyGraph_outputs();
  #ifndef BTH_DEACTIVATE_INITIAL_UNKNOWNS
  /* 2017-08-11 BThiele: Get a strange error when executing the 'cs_BouncingBall.mos'
     example from the 'OMSimulatorModelica' testsuite. In the loop in deactivated
     function below an access to startIndex[0 + 1] results in a strange (very) high
     number which then leads to an access violation.
  */
  getDependencyGraph_initialUnknowns();
  #endif // BTH_DEACTIVATE_INITIAL_UNKNOWNS
}

FMUWrapper::~FMUWrapper()
{
  logTrace();
  fmi2_import_free_instance(fmu);
  fmi2_import_destroy_dllfmu(fmu);
  fmi2_import_free(fmu);
  fmi_import_free_context(context);
  if (boost::filesystem::is_directory(tempDir))
  {
    fmi_import_rmdir(&callbacks, tempDir.c_str());
    logDebug("removed working directory: \"" + tempDir + "\"");
  }
}

double FMUWrapper::getReal(const std::string& var)
{
  logTrace();
  if (!fmu)
    logFatal("FMUWrapper::getReal failed");

  Variable* v = getVariable(var);

  if (!v)
    logFatal("FMUWrapper::getReal failed");

  double value;
  fmi2_import_get_real(fmu, &v->getValueReference(), 1, &value);

  return value;
}

bool FMUWrapper::setRealInput(const std::string& var, double value)
{
  logTrace();
  if (!fmu)
    logFatal("FMUWrapper::setRealInput failed");

  Variable* v = getVariable(var);

  if (!v || !v->isInput() || !v->isTypeReal())
  {
    logError("FMUWrapper::setRealInput: FMU '" + instanceName + "' doesn't contain input real " + var);
    return false;
  }

  fmi2_import_set_real(fmu, &v->getValueReference(), 1, &value);
  return true;
}

bool FMUWrapper::setRealParameter(const std::string& var, double value)
{
  logTrace();
  if (!fmu)
    logFatal("FMUWrapper::setRealParameter failed");

  Variable* v = getVariable(var);

  if (!v || !v->isParameter() || !v->isTypeReal())
  {
    logError("FMUWrapper::setRealParameter: FMU '" + instanceName + "' doesn't contain parameter real " + var);
    return false;
  }

  fmi2_import_set_real(fmu, &v->getValueReference(), 1, &value);
  return true;
}

void FMUWrapper::getDependencyGraph_outputs()
{
  size_t *startIndex, *dependency;
  char* factorKind;

  for (int i = 0; i < allOutputs.size(); i++)
    outputsGraph.addVariable(allVariables[allOutputs[i] - 1]);

  fmi2_import_get_outputs_dependencies(fmu, &startIndex, &dependency, &factorKind);

  if (!startIndex)
  {
    logDebug("FMUWrapper::getDependencyGraph: [" + instanceName + ": " + fmuPath + "] dependencies are not available");
    /* all outputs depend on all inputs */
    for (int i = 0; i < allInputs.size(); i++)
      for (int j = 0; j < allOutputs.size(); j++)
        outputsGraph.addEdge(allVariables[allInputs[i] - 1], allVariables[allOutputs[j] - 1]);
    return;
  }

  for (int i = 0; i < allOutputs.size(); i++)
  {
    if (startIndex[i] == startIndex[i + 1])
    {
      logDebug("FMUWrapper::getDependencyGraph: [" + instanceName + ": " + fmuPath + "] output " + allVariables[allOutputs[i] - 1].getName() + " has no dependencies");
    }
    else if ((startIndex[i] + 1 == startIndex[i + 1]) && (dependency[startIndex[i]] == 0))
    {
      logDebug("FMUWrapper::getDependencyGraph: [" + instanceName + ": " + fmuPath + "] output " + allVariables[allOutputs[i] - 1].getName() + " depends on all");
      for (int j = 0; j < allInputs.size(); j++)
        outputsGraph.addEdge(allVariables[allInputs[j] - 1], allVariables[allOutputs[i] - 1]);
    }
    else
    {
      for (size_t j = startIndex[i]; j < startIndex[i + 1]; j++)
      {
        logDebug("FMUWrapper::getDependencyGraph: [" + instanceName + ": " + fmuPath + "] output " + allVariables[allOutputs[i] - 1].getName() + " depends on " + allVariables[dependency[j] - 1].getName());
        outputsGraph.addEdge(allVariables[dependency[j] - 1], allVariables[allOutputs[i] - 1]);
      }
    }
  }
}

void FMUWrapper::getDependencyGraph_initialUnknowns()
{
  size_t *startIndex, *dependency;
  char* factorKind;

  for (int i = 0; i < initialUnknowns.size(); i++)
    initialUnknownsGraph.addVariable(allVariables[initialUnknowns[i] - 1]);

  fmi2_import_get_initial_unknowns_dependencies(fmu, &startIndex, &dependency, &factorKind);

  if (!startIndex)
  {
    logDebug("FMUWrapper::getDependencyGraph_initialUnknowns: [" + instanceName + ": " + fmuPath + "] dependencies are not available");
    /* all outputs depend on all initial unknowns */
    /* all initial unknowns depend on all inputs */
    for (int i = 0; i < initialUnknowns.size(); i++)
    {
      for (int j = 0; j < allOutputs.size(); j++)
        initialUnknownsGraph.addEdge(allVariables[initialUnknowns[i] - 1], allVariables[allOutputs[j] - 1]);
      for (int j = 0; j < allInputs.size(); j++)
        initialUnknownsGraph.addEdge(allVariables[allInputs[j] - 1], allVariables[initialUnknowns[i] - 1]);
    }
    return;
  }

  for (int i = 0; i < initialUnknowns.size(); i++)
  {
    if (startIndex[i] == startIndex[i + 1])
    {
      logDebug("FMUWrapper::getDependencyGraph_initialUnknowns: [" + instanceName + ": " + fmuPath + "] initial unknown " + allVariables[initialUnknowns[i] - 1].getName() + " has no dependencies");
    }
    else if ((startIndex[i] + 1 == startIndex[i + 1]) && (dependency[startIndex[i]] == 0))
    {
      logDebug("FMUWrapper::getDependencyGraph_initialUnknowns: [" + instanceName + ": " + fmuPath + "] initial unknown " + allVariables[initialUnknowns[i] - 1].getName() + " depends on all");
      for (int j = 0; j < allOutputs.size(); j++)
        initialUnknownsGraph.addEdge(allVariables[initialUnknowns[i] - 1], allVariables[allOutputs[j] - 1]);
      for (int j = 0; j < allInputs.size(); j++)
        initialUnknownsGraph.addEdge(allVariables[allInputs[j] - 1], allVariables[initialUnknowns[i] - 1]);
    }
    else
    {
      for (size_t j = startIndex[i]; j < startIndex[i + 1]; j++)
      {
        logDebug("FMUWrapper::getDependencyGraph_initialUnknowns: [" + instanceName + ": " + fmuPath + "] initial unknown " + allVariables[initialUnknowns[i] - 1].getName() + " depends on " + allVariables[dependency[j] - 1].getName());
        initialUnknownsGraph.addEdge(allVariables[dependency[j] - 1], allVariables[initialUnknowns[i] - 1]);
      }
    }
  }
}

Variable* FMUWrapper::getVariable(const std::string& name)
{
  for (size_t i = 0; i < allVariables.size(); i++)
    if (name == allVariables[i].getName())
      return &allVariables[i];
  return NULL;
}

Variable* FMUWrapper::getVariable(const fmi2_value_reference_t& state_vr)
{
  for (size_t i = 0; i < allVariables.size(); i++)
    if (state_vr == allVariables[i].getValueReference())
      return &allVariables[i];
  return NULL;
}

std::string FMUWrapper::getFMUKind()
{
  if (fmi2_fmu_kind_me == fmuKind) return "FMI 2.0 ME";
  if (fmi2_fmu_kind_cs == fmuKind) return "FMI 2.0 CS";
  if (fmi2_fmu_kind_me_and_cs == fmuKind) return "FMI 2.0 ME & CS";
  return "Unsupported FMU kind";
}

bool FMUWrapper::isFMUKindME()
{
  if (fmi2_fmu_kind_me == fmuKind) return true;
  return false;
}

std::string FMUWrapper::getGUID()
{
  const char* GUID = fmi2_import_get_GUID(fmu);
  return std::string(GUID);
}

std::string FMUWrapper::getGenerationTool()
{
  const char* tool = fmi2_import_get_generation_tool(fmu);
  if (tool)
    return std::string(tool);
  return "<unknown>";
}

void FMUWrapper::do_event_iteration()
{
  eventInfo.newDiscreteStatesNeeded = fmi2_true;
  eventInfo.terminateSimulation = fmi2_false;
  while (eventInfo.newDiscreteStatesNeeded && !eventInfo.terminateSimulation)
    fmi2_import_new_discrete_states(fmu, &eventInfo);
}

void FMUWrapper::enterInitialization(double startTime)
{
  fmi2_status_t fmistatus;

  double* pTolerance = model.getSettings().GetTolerance();
  relativeTolerance = pTolerance ? *pTolerance : fmi2_import_get_default_experiment_tolerance(fmu);
  tcur = startTime;
  fmi2_boolean_t toleranceControlled = fmi2_true;
  fmi2_boolean_t StopTimeDefined = fmi2_true;

  double* pStopTime = model.getSettings().GetStopTime();
  fmi2_real_t tend = pStopTime ? *pStopTime : fmi2_import_get_default_experiment_stop(fmu);

  logDebug("start time: " + toString(tcur));
  logDebug("relative tolerance: " + toString(relativeTolerance));

  if (fmi2_fmu_kind_me == fmuKind)
  {
    fmistatus = fmi2_import_setup_experiment(fmu, toleranceControlled, relativeTolerance, tcur, StopTimeDefined, tend);
    if (fmi2_status_ok != fmistatus) logFatal("fmi2_import_setup_experiment failed");

    fmistatus = fmi2_import_enter_initialization_mode(fmu);
    if (fmi2_status_ok != fmistatus) logFatal("fmi2_import_enter_initialization_mode failed");
  }
  else if (fmi2_fmu_kind_cs == fmuKind || fmi2_fmu_kind_me_and_cs == fmuKind)
  {
    fmistatus = fmi2_import_setup_experiment(fmu, toleranceControlled, relativeTolerance, tcur, StopTimeDefined, tend);
    if (fmi2_status_ok != fmistatus) logFatal("fmi2_import_setup_experiment failed");

    fmistatus = fmi2_import_enter_initialization_mode(fmu);
    if (fmi2_status_ok != fmistatus) logFatal("fmi2_import_enter_initialization_mode failed");
  }
  else
    logFatal("Unsupported FMU kind");
}

void FMUWrapper::exitInitialization()
{
  fmi2_status_t fmistatus;

  const char* resultFile = model.getSettings().GetResultFile();
  std::string finalResultFile;
  if (resultFile)
    finalResultFile = std::string(resultFile) + "_" + instanceName + "_res.csv";
  else
    finalResultFile = instanceName + "_res.csv";
  logDebug("result file: " + finalResultFile);

  if (fmi2_fmu_kind_me == fmuKind)
  {
    fmistatus = fmi2_import_exit_initialization_mode(fmu);
    if (fmi2_status_ok != fmistatus) logFatal("fmi2_import_exit_initialization_mode failed");

    terminateSimulation = fmi2_false;
    omsResultFile = new Resultfile(finalResultFile, fmu);

    eventInfo.newDiscreteStatesNeeded = fmi2_false;
    eventInfo.terminateSimulation = fmi2_false;
    eventInfo.nominalsOfContinuousStatesChanged = fmi2_false;
    eventInfo.valuesOfContinuousStatesChanged = fmi2_true;
    eventInfo.nextEventTimeDefined = fmi2_false;
    eventInfo.nextEventTime = -0.0;

    // fmi2_import_exit_initialization_mode leaves FMU in event mode
    do_event_iteration();
    fmi2_import_enter_continuous_time_mode(fmu);
    omsResultFile->emit(tcur);

    callEventUpdate = fmi2_false;

    n_states = fmi2_import_get_number_of_continuous_states(fmu);
    n_event_indicators = fmi2_import_get_number_of_event_indicators(fmu);

    logDebug(toString(n_states) + " states");
    logDebug(toString(n_event_indicators) + " event indicators");

    if (n_states < 1)
      solverMethod = NO_SOLVER;

    states = (double*)calloc(n_states, sizeof(double));
    states_der = (double*)calloc(n_states, sizeof(double));
    states_nominal = (double*)calloc(n_states, sizeof(double));
    event_indicators = (double*)calloc(n_event_indicators, sizeof(double));
    event_indicators_prev = (double*)calloc(n_event_indicators, sizeof(double));

    if (n_states > 0)
    {
      // get states and state derivatives
      fmistatus = fmi2_import_get_continuous_states(fmu, states, n_states);
      if (fmi2_status_ok != fmistatus) logFatal("fmi2_import_get_continuous_states failed");
      fmistatus = fmi2_import_get_derivatives(fmu, states_der, n_states);
      if (fmi2_status_ok != fmistatus) logFatal("fmi2_import_get_derivatives failed");
      fmistatus = fmi2_import_get_nominals_of_continuous_states(fmu, states_nominal, n_states);
      if (fmi2_status_ok != fmistatus) logFatal("fmi2_import_get_nominals_of_continuous_states failed");
    }
    fmistatus = fmi2_import_get_event_indicators(fmu, event_indicators, n_event_indicators);
    if (fmi2_status_ok != fmistatus) logFatal("fmi2_import_get_event_indicators failed");

    if (n_states < 1)
    {
      this->solverMethod = NO_SOLVER;
      logDebug("FMU '" + instanceName + "' doesn't contain any state.");
    }

    // initialize solver data
    if (NO_SOLVER == solverMethod)
    {
      if (n_states > 0)
        logFatal("No solver specified for FMU '" + instanceName + "'");
    }
    else if (EXPLICIT_EULER == solverMethod)
    {
    }
    else if (CVODE == solverMethod)
    {
      solverData.cvode.y = N_VNew_Serial(static_cast<long>(n_states));
      if (!solverData.cvode.y) logFatal("SUNDIALS_ERROR: N_VNew_Serial() failed - returned NULL pointer");
      for (size_t i = 0; i < n_states; ++i)
        NV_Ith_S(solverData.cvode.y, i) = states[i];

      solverData.cvode.abstol = N_VNew_Serial(static_cast<long>(n_states));
      if (!solverData.cvode.abstol) logFatal("SUNDIALS_ERROR: N_VNew_Serial() failed - returned NULL pointer");
      for (size_t i = 0; i < n_states; ++i)
        NV_Ith_S(solverData.cvode.abstol, i) = 0.01*relativeTolerance*states_nominal[i];

      // Call CVodeCreate to create the solver memory and specify the
      // Backward Differentiation Formula and the use of a Newton iteration
      solverData.cvode.mem = CVodeCreate(CV_BDF, CV_NEWTON);
      if (!solverData.cvode.mem) logFatal("SUNDIALS_ERROR: CVodeCreate() failed - returned NULL pointer");

      int flag = CVodeSetUserData(solverData.cvode.mem, (void*)this);
      if (flag < 0) logFatal("SUNDIALS_ERROR: CVodeSetUserData() failed with flag = " + toString(flag));

      // Call CVodeInit to initialize the integrator memory and specify the
      // user's right hand side function in y'=cvode_rhs(t,y), the inital time T0, and
      // the initial dependent variable vector y.
      flag = CVodeInit(solverData.cvode.mem, cvode_rhs, tcur, solverData.cvode.y);
      if (flag < 0) logFatal("SUNDIALS_ERROR: CVodeInit() failed with flag = " + toString(flag));

      // Call CVodeSVtolerances to specify the scalar relative tolerance
      // and vector absolute tolerances
      flag = CVodeSVtolerances(solverData.cvode.mem, relativeTolerance, solverData.cvode.abstol);
      if (flag < 0) logFatal("SUNDIALS_ERROR: CVodeSVtolerances() failed with flag = " + toString(flag));

      // Call CVDense to specify the CVDENSE dense linear solver */
      flag = CVDense(solverData.cvode.mem, static_cast<long>(n_states));
      if (flag < 0) logFatal("SUNDIALS_ERROR: CVDense() failed with flag = " + toString(flag));

      if (model.getSettings().GetStartTime() && model.getSettings().GetStopTime())
      {
        double max_h = (*model.getSettings().GetStopTime() - *model.getSettings().GetStartTime()) / 10.0;
        logInfo("maximum step size for '" + instanceName + "': " + toString(max_h));
        flag = CVodeSetMaxStep(solverData.cvode.mem, max_h);
        if (flag < 0) logFatal("SUNDIALS_ERROR: CVodeSetMaxStep() failed with flag = " + toString(flag));
      }

      // further settings from cpp runtime
      flag = CVodeSetInitStep(solverData.cvode.mem, 1e-6);        // INITIAL STEPSIZE
      if (flag < 0) logFatal("SUNDIALS_ERROR: CVodeSetInitStep() failed with flag = " + toString(flag));
      flag = CVodeSetMaxOrd(solverData.cvode.mem, 5);             // MAXIMUM ORDER
      if (flag < 0) logFatal("SUNDIALS_ERROR: CVodeSetMaxOrd() failed with flag = " + toString(flag));
      flag = CVodeSetMaxConvFails(solverData.cvode.mem, 100);     // MAXIMUM NUMBER OF NONLINEAR CONVERGENCE FAILURES
      if (flag < 0) logFatal("SUNDIALS_ERROR: CVodeSetMaxConvFails() failed with flag = " + toString(flag));
      flag = CVodeSetStabLimDet(solverData.cvode.mem, TRUE);      // STABILITY DETECTION
      if (flag < 0) logFatal("SUNDIALS_ERROR: CVodeSetStabLimDet() failed with flag = " + toString(flag));
      flag = CVodeSetMinStep(solverData.cvode.mem, 1e-12);        // MINIMUM STEPSIZE
      if (flag < 0) logFatal("SUNDIALS_ERROR: CVodeSetMinStep() failed with flag = " + toString(flag));
      flag = CVodeSetMaxNonlinIters(solverData.cvode.mem, 5);     // MAXIMUM NUMBER OF ITERATIONS
      if (flag < 0) logFatal("SUNDIALS_ERROR: CVodeSetMaxNonlinIters() failed with flag = " + toString(flag));
      flag = CVodeSetMaxErrTestFails(solverData.cvode.mem, 100);  // MAXIMUM NUMBER OF ERROR TEST FAILURES
      if (flag < 0) logFatal("SUNDIALS_ERROR: CVodeSetMaxErrTestFails() failed with flag = " + toString(flag));
      flag = CVodeSetMaxNumSteps(solverData.cvode.mem, 1000);     // MAXIMUM NUMBER OF STEPS
      if (flag < 0) logFatal("SUNDIALS_ERROR: CVodeSetMaxNumSteps() failed with flag = " + toString(flag));
    }
    else
      logFatal("Unknown solver method");
  }
  else if (fmi2_fmu_kind_cs == fmuKind || fmi2_fmu_kind_me_and_cs == fmuKind)
  {
    fmistatus = fmi2_import_exit_initialization_mode(fmu);
    if (fmi2_status_ok != fmistatus) logFatal("fmi2_import_exit_initialization_mode failed");

    omsResultFile = new Resultfile(finalResultFile, fmu);
    omsResultFile->emit(tcur);
  }
  else
    logFatal("Unsupported FMU kind");
}

void FMUWrapper::terminate()
{
  if (fmi2_fmu_kind_me == fmuKind)
  {
    // free solver data
    if (NO_SOLVER == solverMethod)
    {

    }
    else if (EXPLICIT_EULER == solverMethod)
    {
    }
    else if (CVODE == solverMethod)
    {
      long int nst, nfe, nsetups, nni, ncfn, netf;
      int flag;

      flag = CVodeGetNumSteps(solverData.cvode.mem, &nst);
      if (flag < 0) logFatal("SUNDIALS_ERROR: CVodeGetNumSteps() failed with flag = " + toString(flag));
      flag = CVodeGetNumRhsEvals(solverData.cvode.mem, &nfe);
      if (flag < 0) logFatal("SUNDIALS_ERROR: CVodeGetNumRhsEvals() failed with flag = " + toString(flag));
      flag = CVodeGetNumLinSolvSetups(solverData.cvode.mem, &nsetups);
      if (flag < 0) logFatal("SUNDIALS_ERROR: CVodeGetNumLinSolvSetups() failed with flag = " + toString(flag));
      flag = CVodeGetNumErrTestFails(solverData.cvode.mem, &netf);
      if (flag < 0) logFatal("SUNDIALS_ERROR: CVodeGetNumErrTestFails() failed with flag = " + toString(flag));
      flag = CVodeGetNumNonlinSolvIters(solverData.cvode.mem, &nni);
      if (flag < 0) logFatal("SUNDIALS_ERROR: CVodeGetNumNonlinSolvIters() failed with flag = " + toString(flag));
      flag = CVodeGetNumNonlinSolvConvFails(solverData.cvode.mem, &ncfn);
      if (flag < 0) logFatal("SUNDIALS_ERROR: CVodeGetNumNonlinSolvConvFails() failed with flag = " + toString(flag));

      logInfo("Final Statistics for '" + instanceName + "':");
      logInfo("NumSteps = " + toString(nst) + " NumRhsEvals  = " + toString(nfe) + " NumLinSolvSetups = " + toString(nsetups));
      logInfo("NumNonlinSolvIters = " + toString(nni) + " NumNonlinSolvConvFails = " + toString(ncfn) + " NumErrTestFails = " + toString(netf));

      N_VDestroy_Serial(solverData.cvode.y);
      N_VDestroy_Serial(solverData.cvode.abstol);
      CVodeFree(&(solverData.cvode.mem));
    }
    else
      logFatal("Unknown solver method");

    // free common data
    free(states);
    free(states_der);
    free(states_nominal);
  }
  if (omsResultFile) delete omsResultFile;

  fmi2_status_t fmistatus = fmi2_import_terminate(fmu);
  if (fmi2_status_ok != fmistatus) logFatal("fmi2_import_terminate failed");
}

void FMUWrapper::reset()
{
  if (fmi2_fmu_kind_me == fmuKind)
  {
    // free solver data
    if (NO_SOLVER == solverMethod)
    {
    }
    else if (EXPLICIT_EULER == solverMethod)
    {
    }
    else if (CVODE == solverMethod)
    {
      N_VDestroy_Serial(solverData.cvode.y);
      N_VDestroy_Serial(solverData.cvode.abstol);
      CVodeFree(&(solverData.cvode.mem));
    }
    else
      logFatal("Unknown solver method");

    free(states);
    free(states_der);
    free(states_nominal);
  }
  if (omsResultFile) delete omsResultFile;

  fmi2_status_t fmistatus = fmi2_import_reset(fmu);
  if (fmi2_status_ok != fmistatus) logFatal("fmi2_import_reset failed");
}

void FMUWrapper::doStep(double stopTime)
{
  fmi2_status_t fmistatus;
  fmi2_real_t hdef = model.getSettings().GetCommunicationInterval() ? *(model.getSettings().GetCommunicationInterval()) : 1e-2;

  if (fmi2_fmu_kind_me == fmuKind)
  {
    // main simulation loop
    fmi2_real_t hcur = hdef;
    fmi2_real_t tlast = tcur;
    while ((tcur < stopTime) && (!(eventInfo.terminateSimulation || terminateSimulation)))
    {
      fmistatus = fmi2_import_set_time(fmu, tcur);
      if (fmi2_status_ok != fmistatus) logFatal("fmi2_import_set_time failed");

      // swap event_indicators and event_indicators_prev
      {
        fmi2_real_t *temp = event_indicators;
        event_indicators = event_indicators_prev;
        event_indicators_prev = temp;

        fmistatus = fmi2_import_get_event_indicators(fmu, event_indicators, n_event_indicators);
        if (fmi2_status_ok != fmistatus) logFatal("fmi2_import_get_event_indicators failed");
      }

      // check if an event indicator has triggered
      int zero_crossing_event = 0;
      for (int k = 0; k < n_event_indicators; k++)
      {
        if ((event_indicators[k] > 0) != (event_indicators_prev[k] > 0))
        {
          zero_crossing_event = 1;
          break;
        }
      }

      // handle events
      if (callEventUpdate || zero_crossing_event || (eventInfo.nextEventTimeDefined && tcur == eventInfo.nextEventTime))
      {
        fmistatus = fmi2_import_enter_event_mode(fmu);
        if (fmi2_status_ok != fmistatus) logFatal("fmi2_import_enter_event_mode failed");

        do_event_iteration();

        fmistatus = fmi2_import_enter_continuous_time_mode(fmu);
        if (fmi2_status_ok != fmistatus) logFatal("fmi2_import_enter_continuous_time_mode failed");
        if (n_states > 0)
        {
          fmistatus = fmi2_import_get_continuous_states(fmu, states, n_states);
          if (fmi2_status_ok != fmistatus) logFatal("fmi2_import_get_continuous_states failed");
          fmistatus = fmi2_import_get_derivatives(fmu, states_der, n_states);
          if (fmi2_status_ok != fmistatus) logFatal("fmi2_import_get_derivatives failed");
        }
        fmistatus = fmi2_import_get_event_indicators(fmu, event_indicators, n_event_indicators);
        if (fmi2_status_ok != fmistatus) logFatal("fmi2_import_get_event_indicators failed");
        omsResultFile->emit(tcur);

        if (CVODE == solverMethod)
        {
          for (size_t i = 0; i < n_states; ++i)
            NV_Ith_S(solverData.cvode.y, i) = states[i];
          int flag = CVodeReInit(solverData.cvode.mem, tcur, solverData.cvode.y);
          if (flag < 0) logFatal("SUNDIALS_ERROR: CVodeReInit() failed with flag = " + toString(flag));
        }
      }

      // calculate next time step
      tlast = tcur;
      tcur += hdef;
      if (eventInfo.nextEventTimeDefined && (tcur >= eventInfo.nextEventTime))
        tcur = eventInfo.nextEventTime;

      hcur = tcur - tlast;

      if (tcur > stopTime - hcur / 1e16)
      {
        // adjust final step size
        tcur = stopTime;
        hcur = tcur - tlast;
      }

      // integrate using specified solver
      if (NO_SOLVER == solverMethod)
      {
      }
      else if (EXPLICIT_EULER == solverMethod)
      {
        for (int k = 0; k < n_states; k++)
          states[k] = states[k] + hcur*states_der[k];
      }
      else if (CVODE == solverMethod)
      {
        double cvode_time = tlast;
        while (cvode_time < tcur)
        {
          int flag = CVode(solverData.cvode.mem, tcur, solverData.cvode.y, &cvode_time, CV_ONE_STEP);
          if (flag < 0) logFatal("SUNDIALS_ERROR: CVode() failed with flag = " + toString(flag));
        }
        tcur = cvode_time;
      }
      else
        logFatal("Unknown solver method");

      if (NO_SOLVER != solverMethod)
      {
        // set states
        fmistatus = fmi2_import_set_continuous_states(fmu, states, n_states);
        if (fmi2_status_ok != fmistatus) logFatal("fmi2_import_set_continuous_states failed");
        // get state derivatives
        fmistatus = fmi2_import_get_derivatives(fmu, states_der, n_states);
        if (fmi2_status_ok != fmistatus) logFatal("fmi2_import_get_derivatives failed");
      }

      // step is complete
      fmistatus = fmi2_import_completed_integrator_step(fmu, fmi2_true, &callEventUpdate, &terminateSimulation);
      if (fmi2_status_ok != fmistatus) logFatal("fmi2_import_completed_integrator_step failed");

      omsResultFile->emit(tcur);
    }
  }
  else if (fmi2_fmu_kind_cs == fmuKind || fmi2_fmu_kind_me_and_cs == fmuKind)
  {
    while (tcur < stopTime)
    {
      fmistatus = fmi2_import_do_step(fmu, tcur, hdef, fmi2_true);
      tcur += hdef;
      omsResultFile->emit(tcur);
    }
  }
}

void FMUWrapper::SetSolverMethod(const std::string& solverMethod)
{
  if (!isFMUKindME())
    logError("FMUWrapper::SetSolverMethod: Solver method can only be specified for FMU ME");

  if (solverMethod == "none")
    this->solverMethod = NO_SOLVER;
  if (solverMethod == "euler")
    this->solverMethod = EXPLICIT_EULER;
  else if (solverMethod == "cvode")
    this->solverMethod = CVODE;
  else
    logError("Settings::SetSolverMethod: Unknown solver method '" + solverMethod + "'");
}

std::string FMUWrapper::GetSolverMethodString()
{
  switch (solverMethod)
  {
  case NO_SOLVER:
    return std::string("none");
  case EXPLICIT_EULER:
    return std::string("euler");
  case CVODE:
    return std::string("cvode");
  default:
    logError("FMUWrapper::GetSolverMethodString: Unknown solver method " + toString(solverMethod));
    return std::string("unknown");
  }
}
