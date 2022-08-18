/*
BSD 3-Clause License

Copyright (c) 2020, The Regents of the University of Minnesota

All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

* Neither the name of the copyright holder nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
#include <math.h>
#include <stdlib.h>
#include <time.h>

#include <Eigen/Sparse>
#include <Eigen/SparseLU>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <queue>
#include <sstream>
#include <string>
#include <vector>

#include "get_power.h"
#include "get_voltage.h"
#include "gmat.h"
#include "ir_solver.h"
#include "node.h"
#include "odb/db.h"

namespace psm {
using odb::dbBlock;
using odb::dbBox;
using odb::dbChip;
using odb::dbDatabase;
using odb::dbInst;
using odb::dbNet;
using odb::dbRow;
using odb::dbSBox;
using odb::dbSet;
using odb::dbSigType;
using odb::dbSWire;
using odb::dbTech;
using odb::dbTechLayer;
using odb::dbTechLayerDir;
using odb::dbTechVia;
using odb::dbVia;
using odb::dbViaParams;

using std::endl;
using std::get;
using std::ifstream;
using std::make_pair;
using std::make_tuple;
using std::map;
using std::ofstream;
using std::pair;
using std::queue;
using std::setprecision;
using std::stod;
using std::string;
using std::stringstream;
using std::to_string;
using std::tuple;
using std::vector;

using Eigen::Map;
using Eigen::SparseLU;
using Eigen::SparseMatrix;
using Eigen::Success;
using Eigen::VectorXd;

IRSolver::IRSolver(odb::dbDatabase* db,
                   sta::dbSta* sta,
                   utl::Logger* logger,
                   std::string vsrc_loc,
                   std::string power_net,
                   std::string out_file,
                   std::string em_out_file,
                   std::string spice_out_file,
                   bool em_analyze,
                   int bump_pitch_x,
                   int bump_pitch_y,
                   float node_density_um,
                   int node_density_factor_user,
                   const std::map<std::string, float>& net_voltage_map)
{
  db_ = db;
  sta_ = sta;
  logger_ = logger;
  vsrc_file_ = vsrc_loc;
  power_net_ = power_net;
  out_file_ = out_file;
  em_out_file_ = em_out_file;
  em_flag_ = em_analyze;
  spice_out_file_ = spice_out_file;
  bump_pitch_x_ = bump_pitch_x;
  bump_pitch_y_ = bump_pitch_y;
  node_density_um_ = node_density_um;
  node_density_factor_user_ = node_density_factor_user;
  net_voltage_map_ = net_voltage_map;
}

IRSolver::~IRSolver()
{
}

//! Returns the created G matrix for the design
/*
 * \return G Matrix
 */
GMat* IRSolver::getGMat()
{
  return Gmat_.get();
}

//! Returns current map represented as a 1D vector
/*
 * \return J vector
 */
vector<double> IRSolver::getJ()
{
  return J_;
}

//! Function to solve for voltage using SparseLU
void IRSolver::solveIR()
{
  if (!connection_) {
    logger_->warn(utl::PSM,
                  8,
                  "Powergrid is not connected to all instances, therefore the "
                  "IR Solver may not be accurate. LVS may also fail.");
  }
  const int unit_micron = db_->getTech()->getDbUnitsPerMicron();
  CscMatrix* Gmat = Gmat_->getGMat();
  // fill A
  double* values = &(Gmat->values[0]);
  int* row_idx = &(Gmat->row_idx[0]);
  int* col_ptr = &(Gmat->col_ptr[0]);
  Map<SparseMatrix<double>> A(Gmat->num_rows,
                              Gmat->num_cols,
                              Gmat->nnz,
                              col_ptr,  // read-write
                              row_idx,
                              values);
  vector<double> J = getJ();
  Map<VectorXd> b(J.data(), J.size());
  SparseLU<SparseMatrix<double>> solver;
  debugPrint(logger_, utl::PSM, "IR Solver", 1, "Factorizing the G matrix");
  solver.compute(A);
  if (solver.info() != Success) {
    // decomposition failed
    logger_->error(
        utl::PSM,
        10,
        "LU factorization of the G Matrix failed. SparseLU solver message: {}.",
        solver.lastErrorMessage());
  }
  debugPrint(
      logger_, utl::PSM, "IR Solver", 1, "Solving system of equations GV=J");
  VectorXd x = solver.solve(b);
  if (solver.info() != Success) {
    // solving failed
    logger_->error(utl::PSM, 12, "Solving V = inv(G)*J failed.");
  } else {
    debugPrint(logger_,
               utl::PSM,
               "IR Solver",
               1,
               "Solving system of equations GV=J complete");
  }
  ofstream ir_report;
  ir_report.open(out_file_);
  ir_report << "Instance name, "
            << " X location, "
            << " Y location, "
            << " Voltage "
            << "\n";
  const int num_nodes = Gmat_->getNumNodes();
  int node_num = 0;
  double sum_volt = 0;
  wc_voltage = supply_voltage_src;
  while (node_num < num_nodes) {
    Node* node = Gmat_->getNode(node_num);
    const double volt = x(node_num);
    sum_volt = sum_volt + volt;
    if (power_net_type_ == dbSigType::POWER) {
      if (volt < wc_voltage) {
        wc_voltage = volt;
      }
    } else {
      if (volt > wc_voltage) {
        wc_voltage = volt;
      }
    }
    node->setVoltage(volt);
    node_num++;
    if (node->hasInstances()) {
      const Point node_loc = node->getLoc();
      const float loc_x = node_loc.getX() / ((float) unit_micron);
      const float loc_y = node_loc.getY() / ((float) unit_micron);
      if (out_file_ != "") {
        for (dbInst* inst : node->getInstances()) {
          ir_report << inst->getName() << ", " << loc_x << ", " << loc_y << ", "
                    << setprecision(6) << volt << "\n";
        }
      }
    }
  }
  ir_report << endl;
  ir_report.close();
  avg_voltage = sum_volt / num_nodes;
  if (em_flag_) {
    DokMatrix* Gmat_dok = Gmat_->getGMatDOK();
    int resistance_number = 0;
    max_cur = 0;
    double sum_cur = 0;
    ofstream em_report;
    if (em_out_file_ != "") {
      em_report.open(em_out_file_);
      em_report << "Segment name, "
                << " Current, "
                << " Node 1, "
                << " Node 2 "
                << "\n";
    }
    Point node_loc;
    for (auto [loc, value] : Gmat_dok->values) {
      const NodeIdx col = loc.first;
      const NodeIdx row = loc.second;
      if (col <= row) {
        continue;  // ignore lower half and diagonal as matrix is symmetric
      }
      const double cond = value;  // get cond value
      if (abs(cond) < 1e-15) {    // ignore if an empty cell
        continue;
      }
      const string net_name = power_net_;
      if (col < num_nodes) {  // resistances
        const double resistance = -1 / cond;

        const Node* node1 = Gmat_->getNode(col);
        const Node* node2 = Gmat_->getNode(row);
        node_loc = node1->getLoc();
        const int x1 = node_loc.getX();
        const int y1 = node_loc.getY();
        const int l1 = node1->getLayerNum();
        const string node1_name = net_name + "_" + to_string(x1) + "_"
                                  + to_string(y1) + "_" + to_string(l1);

        node_loc = node2->getLoc();
        int x2 = node_loc.getX();
        int y2 = node_loc.getY();
        int l2 = node2->getLayerNum();
        string node2_name = net_name + "_" + to_string(x2) + "_" + to_string(y2)
                            + "_" + to_string(l2);

        const string segment_name = "seg_" + to_string(resistance_number);

        const double v1 = node1->getVoltage();
        const double v2 = node2->getVoltage();
        double seg_cur = (v1 - v2) / resistance;
        sum_cur += abs(seg_cur);
        if (em_out_file_ != "") {
          em_report << segment_name << ", " << setprecision(3) << seg_cur
                    << ", " << node1_name << ", " << node2_name << endl;
        }
        seg_cur = abs(seg_cur);
        if (seg_cur > max_cur) {
          max_cur = seg_cur;
        }
        resistance_number++;
      }
    }  // for gmat values
    avg_cur = sum_cur / resistance_number;
    num_res = resistance_number;

  }  // enable em
}

//! Function to add C4 bumps to the G matrix
bool IRSolver::addC4Bump()
{
  if (C4Bumps_.size() == 0) {
    logger_->error(utl::PSM, 14, "Number of voltage sources cannot be 0.");
  }
  logger_->info(
      utl::PSM, 64, "Number of voltage sources = {}.", C4Bumps_.size());
  size_t it = 0;
  for (auto [node_loc, voltage_value] : C4Nodes_) {
    Gmat_->addC4Bump(node_loc, it++);  // add the  bump
    J_.push_back(voltage_value);       // push back  vdd
  }
  return true;
}

//! Function that parses the Vsrc file
void IRSolver::readC4Data()
{
  const int unit_micron = (db_->getTech())->getDbUnitsPerMicron();
  if (vsrc_file_ != "") {
    logger_->info(utl::PSM,
                  15,
                  "Reading location of VDD and VSS sources from {}.",
                  vsrc_file_);
    ifstream file(vsrc_file_);
    string line = "";
    // Iterate through each line and split the content using delimiter
    while (getline(file, line)) {
      int x = -1, y = -1, size = -1;
      stringstream X(line);
      string val;
      for (int i = 0; i < 4; ++i) {
        getline(X, val, ',');
        if (i == 0) {
          x = (int) (unit_micron * stod(val));
        } else if (i == 1) {
          y = (int) (unit_micron * stod(val));
        } else if (i == 2) {
          size = (int) (unit_micron * stod(val));
        } else {
          supply_voltage_src = stod(val);
        }
      }
      if (x == -1 || y == -1 || size == -1) {
        logger_->error(utl::PSM, 75, "Expected four values on line: {}", line);
      } else {
        C4Bumps_.push_back({x, y, size, supply_voltage_src});
      }
    }
    file.close();
  } else {
    logger_->warn(utl::PSM,
                  16,
                  "Voltage pad location (VSRC) file not specified, defaulting "
                  "pad location to checkerboard pattern on core area.");
    dbChip* chip = db_->getChip();
    dbBlock* block = chip->getBlock();
    odb::Rect coreRect = block->getCoreArea();
    const int coreW = coreRect.xMax() - coreRect.xMin();
    const int coreL = coreRect.yMax() - coreRect.yMin();
    const odb::Rect dieRect = block->getDieArea();
    const int offset_x = coreRect.xMin() - dieRect.xMin();
    const int offset_y = coreRect.yMin() - dieRect.yMin();
    if (bump_pitch_x_ == 0) {
      bump_pitch_x_ = bump_pitch_default_ * unit_micron;
      logger_->warn(
          utl::PSM,
          17,
          "X direction bump pitch is not specified, defaulting to {}um.",
          bump_pitch_default_);
    }
    if (bump_pitch_y_ == 0) {
      bump_pitch_y_ = bump_pitch_default_ * unit_micron;
      logger_->warn(
          utl::PSM,
          18,
          "Y direction bump pitch is not specified, defaulting to {}um.",
          bump_pitch_default_);
    }
    if (!net_voltage_map_.empty() && net_voltage_map_.count(power_net_) > 0) {
      supply_voltage_src = net_voltage_map_.at(power_net_);
    } else {
      logger_->warn(
          utl::PSM, 19, "Voltage on net {} is not explicitly set.", power_net_);
      const pair<double, double> supply_voltages = getSupplyVoltage();
      dbNet* power_net = block->findNet(power_net_.data());
      if (power_net == NULL) {
        logger_->error(utl::PSM,
                       20,
                       "Cannot find net {} in the design. Please provide a "
                       "valid VDD/VSS net.",
                       power_net_);
      }
      power_net_type_ = power_net->getSigType();
      if (power_net_type_ == dbSigType::GROUND) {
        supply_voltage_src = supply_voltages.second;
        logger_->warn(utl::PSM,
                      21,
                      "Using voltage {:4.3f}V for ground network.",
                      supply_voltage_src);
      } else {
        supply_voltage_src = supply_voltages.first;
        logger_->warn(utl::PSM,
                      22,
                      "Using voltage {:4.3f}V for VDD network.",
                      supply_voltage_src);
      }
    }
    if (coreW < bump_pitch_x_ || coreL < bump_pitch_y_) {
      float to_micron = 1.0f / unit_micron;
      const int x_cor = coreW / 2 + offset_x;
      const int y_cor = coreL / 2 + offset_y;
      logger_->warn(utl::PSM,
                    63,
                    "Specified bump pitches of {:4.3f} and {:4.3f} are less "
                    "than core width of {:4.3f} or core height of {:4.3f}. "
                    "Changing bump location to the center of the die at "
                    "({:4.3f}, {:4.3f}).",
                    bump_pitch_x_ * to_micron,
                    bump_pitch_y_ * to_micron,
                    coreW * to_micron,
                    coreL * to_micron,
                    x_cor * to_micron,
                    y_cor * to_micron);
      C4Bumps_.push_back(
          {x_cor, y_cor, bump_size_ * unit_micron, supply_voltage_src});
    }
    const int num_b_x = coreW / bump_pitch_x_;
    const int centering_offset_x = (coreW - (num_b_x - 1) * bump_pitch_x_) / 2;
    const int num_b_y = coreL / bump_pitch_y_;
    const int centering_offset_y = (coreL - (num_b_y - 1) * bump_pitch_y_) / 2;
    logger_->warn(utl::PSM,
                  65,
                  "VSRC location not specified, using default checkerboard "
                  "pattern with one VDD every size bumps in x-direction and "
                  "one in two bumps in the y-direction");
    for (int i = 0; i < num_b_y; i++) {
      for (int j = 0; j < num_b_x; j = j + 6) {
        const int x_cor = (bump_pitch_x_ * j) + (((2 * i) % 6) * bump_pitch_x_)
                          + offset_x + centering_offset_x;
        const int y_cor = (bump_pitch_y_ * i) + offset_y + centering_offset_y;
        if (x_cor <= coreW && y_cor <= coreL) {
          C4Bumps_.push_back(
              {x_cor, y_cor, bump_size_ * unit_micron, supply_voltage_src});
        }
      }
    }
  }
}

//! Function to create a J vector from the current map
bool IRSolver::createJ()
{  // take current_map as an input?
  const int num_nodes = Gmat_->getNumNodes();
  J_.resize(num_nodes, 0);

  for (auto [inst, power] : getPower()) {
    if (!inst->getPlacementStatus().isPlaced()) {
      logger_->warn(utl::PSM,
                    71,
                    "Instance {} is not placed. Therefore, the"
                    " power drawn by this instance is not considered for IR "
                    " drop estimation. Please run analyze_power_grid after "
                    "instances are placed.",
                    inst->getName());
      continue;
    }
    int x, y;
    inst->getLocation(x, y);
    const int l = bottom_layer_;  // attach to the bottom most routing layer
    // Special condition to distribute power across multiple nodes for macro
    // blocks
    if (inst->isBlock() || inst->isPad()) {
      dbBox* inst_bBox = inst->getBBox();
      std::set<int> pin_layers;
      auto iterms = inst->getITerms();
      // Find the pin layers for the macro
      for (auto&& iterm : iterms) {
        if (iterm->getSigType() == power_net_type_) {
          auto mterm = iterm->getMTerm();
          for (auto mpin : mterm->getMPins()) {
            for (auto box : mpin->getGeometry()) {
              dbTechLayer* pin_layer = box->getTechLayer();
              pin_layers.insert(pin_layer->getRoutingLevel());
            }
          }
        }
      }
      // Search for all nodes within the macro boundary
      vector<Node*> nodes_J;
      for (auto ll : pin_layers) {
        vector<Node*> nodes_J_l = Gmat_->getNodes(ll,
                                                  inst_bBox->xMin(),
                                                  inst_bBox->xMax(),
                                                  inst_bBox->yMin(),
                                                  inst_bBox->yMax());
        nodes_J.insert(nodes_J.end(), nodes_J_l.begin(), nodes_J_l.end());
      }
      double num_nodes = nodes_J.size();
      // If nodes are not found on the pin layers we search for the lowest
      // metal layer that overlaps the macro
      if (num_nodes == 0) {
        const int max_l
            = *std::max_element(pin_layers.begin(), pin_layers.end());
        for (int pl = bottom_layer_ + 1; pl <= top_layer_; pl++) {
          nodes_J = Gmat_->getNodes(pl,
                                    inst_bBox->xMin(),
                                    inst_bBox->xMax(),
                                    inst_bBox->yMin(),
                                    inst_bBox->yMax());
          num_nodes = nodes_J.size();
          if (num_nodes > 0) {
            logger_->warn(
                utl::PSM,
                74,
                "No nodes found in macro bounding box for Instance {} "
                "for the pin layer at routing level {}. Using layer {}.",
                inst->getName(),
                max_l,
                pl);
            break;
          }
        }
        // If nodes are still not found we connect to the neartest node on the
        // highest pin layer with a warning
        if (num_nodes == 0) {
          Node* node_J = Gmat_->getNode(x, y, max_l, true);
          nodes_J = {node_J};
          num_nodes = 1.0;
          const Point node_loc = node_J->getLoc();
          logger_->warn(utl::PSM,
                        72,
                        "No nodes found in macro bounding box for Instance {}."
                        "Using nearest node at ({}, {}) on the pin layer at "
                        "routing level {}.",
                        inst->getName(),
                        node_loc.getX(),
                        node_loc.getY(),
                        max_l);
        }
      }
      // Distribute the power across all nodes within the bounding box
      for (auto node_J : nodes_J) {
        node_J->addCurrentSrc(power / num_nodes);
        node_J->addInstance(inst);
      }
      // For normal instances we only attach the current source to one node
    } else {
      Node* node_J = Gmat_->getNode(x, y, l, true);
      const Point node_loc = node_J->getLoc();
      if (abs(node_loc.getX() - x) > node_density_
          || abs(node_loc.getY() - y) > node_density_) {
        logger_->warn(utl::PSM,
                      24,
                      "Instance {}, current node at ({}, {}) at layer {} have "
                      "been moved from ({}, {}).",
                      inst->getName(),
                      node_loc.getX(),
                      node_loc.getY(),
                      l,
                      x,
                      y);
      }
      // Both these lines will change in the future for multiple power domains
      node_J->addCurrentSrc(power);
      node_J->addInstance(inst);
    }
  }
  // Creating the J matrix
  for (int i = 0; i < num_nodes; ++i) {
    const Node* node_J = Gmat_->getNode(i);
    if (power_net_type_ == dbSigType::GROUND) {
      J_[i] = (node_J->getCurrent());
    } else {
      J_[i] = -1 * (node_J->getCurrent());
    }
  }
  debugPrint(logger_, utl::PSM, "IR Solver", 1, "Created J vector");
  return true;
}

//! Function to find and store the upper and lower PDN layers and return a list
// of wires for all PDN tasks
vector<dbSBox*> IRSolver::findPdnWires(dbNet* power_net)
{
  vector<dbSBox*> power_wires;
  // Iterate through all wires till we reach the lowest abstraction level
  for (dbSWire* curSWire : power_net->getSWires()) {
    for (dbSBox* curWire : curSWire->getWires()) {
      // Store wires in an easy to access format as we reuse it multiple times
      power_wires.push_back(curWire);
      int l;
      dbTechLayerDir::Value layer_dir;
      // If the wire is a via get extract the top layer
      // We assume the bottom most layer must have power stripes.
      if (curWire->isVia()) {
        dbTechLayer* via_layer;
        if (curWire->getBlockVia()) {
          via_layer = curWire->getBlockVia()->getTopLayer();
        } else {
          via_layer = curWire->getTechVia()->getTopLayer();
        }
        l = via_layer->getRoutingLevel();
        layer_dir = via_layer->getDirection();
        // If the wire is a power stripe extract the bottom and bottom layer
      } else {
        dbTechLayer* wire_layer = curWire->getTechLayer();
        l = wire_layer->getRoutingLevel();
        layer_dir = wire_layer->getDirection();
        if (l < bottom_layer_) {
          bottom_layer_ = l;
          bottom_layer_dir_ = layer_dir;
        }
      }
      if (l > top_layer_) {
        top_layer_ = l;
        top_layer_dir_ = layer_dir;
      }
    }
  }
  // return the list of wires to be used in all subsequent loops
  return power_wires;
}

//! Function to create the nodes of the G matrix
void IRSolver::createGmatViaNodes(const vector<dbSBox*>& power_wires)
{
  for (auto curWire : power_wires) {
    // For a Via we create the nodes at the top and bottom ends of the via
    if (curWire->isVia()) {
      dbTechLayer* via_bottom_layer;
      dbTechLayer* via_top_layer;
      if (curWire->getBlockVia()) {
        dbVia* via = curWire->getBlockVia();
        via_top_layer = via->getTopLayer();
        via_bottom_layer = via->getBottomLayer();
      } else {
        dbTechVia* via = curWire->getTechVia();
        via_top_layer = via->getTopLayer();
        via_bottom_layer = via->getBottomLayer();
      }
      const Point loc = curWire->getViaXY();
      // TODO: Using a single node for a via requires that the vias are
      // stacked and not staggered, i.e., V1 via cut must overlap either V2
      // via cut or enclosure and connections cannot be made through
      // enclosures only.
      const int lb = via_bottom_layer->getRoutingLevel();
      Gmat_->setNode(loc, lb);
      const int lt = via_top_layer->getRoutingLevel();
      Gmat_->setNode(loc, lt);
    }
  }
}

void IRSolver::createGmatWireNodes(const vector<dbSBox*>& power_wires,
                               const vector<odb::Rect>& macros)
{
  for (auto curWire : power_wires) {
    // For a stripe we create nodes at the ends of the stripes and at a fixed
    // frequency in the lowermost layer.
    if (curWire->isVia())
     continue;
    dbTechLayer* wire_layer = curWire->getTechLayer();
    const int l = wire_layer->getRoutingLevel();
    dbTechLayerDir::Value layer_dir = wire_layer->getDirection();
    if (l == bottom_layer_) {
      layer_dir = dbTechLayerDir::Value::HORIZONTAL;
    }
    int x_loc1, x_loc2, y_loc1, y_loc2;
    if (layer_dir == dbTechLayerDir::Value::HORIZONTAL) {
      y_loc1 = (curWire->yMin() + curWire->yMax()) / 2;
      y_loc2 = y_loc1;
      x_loc1 = curWire->xMin();
      x_loc2 = curWire->xMax();
    } else {
      x_loc1 = (curWire->xMin() + curWire->xMax()) / 2;
      x_loc2 = x_loc1;
      y_loc1 = curWire->yMin();
      y_loc2 = curWire->yMax();
    }
    // For all layers we create the end nodes
    Gmat_->setNode({x_loc1, y_loc1}, l);
    Gmat_->setNode({x_loc2, y_loc2}, l);
    // Special condition: if the stripe ovelaps a macro ensure a node is
    // created
    for (const auto& macro : macros) {
      if (layer_dir == dbTechLayerDir::Value::HORIZONTAL) {
        // y range is withing the marco (min, max)
        if (y_loc1 >= macro.yMin() && y_loc1 <= macro.yMax()) {
          // Both x values outside the macro
          // (Values inside will already have a node at endpoints)
          if (x_loc1 < macro.xMin() && x_loc2 > macro.xMax()) {
            const int x = (macro.xMin() + macro.xMax()) / 2;
            Gmat_->setNode({x, y_loc1}, l);
          }
        }
      } else {
        if (x_loc1 >= macro.xMin() && x_loc1 <= macro.xMax()) {
          if (y_loc1 < macro.yMin() && y_loc2 > macro.yMax()) {
            const int y = (macro.yMin() + macro.yMax()) / 2;
            Gmat_->setNode({x_loc1, y}, l);
          }
        }
      }
    }
    if (l != bottom_layer_)
      continue;

    // special case for bottom layers we design a dense grid at a fixed
    // frequency
    auto node_map = Gmat_->getNodes(l,layer_dir,x_loc1,x_loc2,y_loc1,y_loc2);
    pair<pair<int, int>, Node*> node_prev;
    int v_itr, v_prev, length;
    int i = 0;
    for (auto& node_itr : node_map) {
      v_itr = (node_itr.first).first;
      if (i == 0) {
        // Before the first existing node
        i = 1;
        if (layer_dir == dbTechLayerDir::Value::HORIZONTAL) {
          v_prev= x_loc1;
        } else {
          v_prev= y_loc1;
        }
      } else {
        v_prev = (node_prev.first).first;
      }
      length = v_itr - v_prev;
      if (length > node_density_) {
        for (int v_i = v_prev+ node_density_; v_i < v_itr;
               v_i += node_density_) {
          if (layer_dir == dbTechLayerDir::Value::HORIZONTAL) {
            Gmat_->setNode({v_i, y_loc1}, l);
          } else {
            Gmat_->setNode({x_loc1, v_i}, l);
          }
        }
      }
      node_prev = node_itr;
    }
    // from the last node to the end
    if(i == 1) {
      int v_loc;
      if (layer_dir == dbTechLayerDir::Value::HORIZONTAL) {
        v_loc = x_loc2;
      } else {
        v_loc = y_loc2;
      }
      length = v_loc - v_itr;
      if (length > node_density_) {
        for (int v_i = v_itr + node_density_; v_i < v_loc;
               v_i += node_density_) {
          if (layer_dir == dbTechLayerDir::Value::HORIZONTAL) {
            Gmat_->setNode({v_i, y_loc1}, l);
          } else {
            Gmat_->setNode({x_loc1, v_i}, l);
          }
        }
      }
    }
  }// for power_wires
}

//! Function to create the connections of the G matrix
void IRSolver::createGmatConnections(const vector<dbSBox*>& power_wires,
                                     bool connection_only)
{
  for (auto curWire : power_wires) {
    // For vias we make 3 connections
    // 1) From the top node to the bottom node
    // 2) Nodes within the top enclosure
    // 3) Nodes within the bottom enclosure
    if (curWire->isVia()) {
      bool has_params;
      dbViaParams params;
      dbTechLayer* via_top_layer;
      dbTechLayer* via_bottom_layer;
      if (curWire->getBlockVia()) {
        dbVia* via = curWire->getBlockVia();
        has_params = via->hasParams();
        if (has_params) {
          via->getViaParams(params);
        }
        via_top_layer = via->getTopLayer();
        via_bottom_layer = via->getBottomLayer();
      } else {
        dbTechVia* via = curWire->getTechVia();
        has_params = via->hasParams();
        if (has_params) {
          via->getViaParams(params);
        }
        via_top_layer = via->getTopLayer();
        via_bottom_layer = via->getBottomLayer();
      }
      int num_via_rows = 1;
      int num_via_cols = 1;
      int x_cut_size = 0;
      int y_cut_size = 0;
      int x_bottom_enclosure = 0;
      int y_bottom_enclosure = 0;
      int x_top_enclosure = 0;
      int y_top_enclosure = 0;
      if (has_params) {
        num_via_rows = params.getNumCutRows();
        num_via_cols = params.getNumCutCols();
        x_cut_size = params.getXCutSize();
        y_cut_size = params.getYCutSize();
        x_bottom_enclosure = params.getXBottomEnclosure();
        y_bottom_enclosure = params.getYBottomEnclosure();
        x_top_enclosure = params.getXTopEnclosure();
        y_top_enclosure = params.getYTopEnclosure();
      }
      int x, y;
      curWire->getViaXY(x, y);

      // Find the resistance of the via
      const double R = via_bottom_layer->getUpperLayer()->getResistance()
                       / (num_via_rows * num_via_cols);
      if (!checkValidR(R) && !connection_only) {
        logger_->error(utl::PSM,
                       35,
                       "{} resistance not found in DB. Check the LEF or "
                       "set it using the 'set_layer_rc' command.",
                       via_bottom_layer->getName());
      }
      // Find the nodes of the via
      const int bot_l = via_bottom_layer->getRoutingLevel();
      const Node* node_bot
          = Gmat_->getNode(x, y, bot_l, bot_l == bottom_layer_);
      const Point bot_node_loc = node_bot->getLoc();
      if (abs(bot_node_loc.getX() - x) > node_density_
          || abs(bot_node_loc.getY() - y) > node_density_) {
        logger_->warn(utl::PSM,
                      32,
                      "Node at ({}, {}) and layer {} moved from ({}, {}).",
                      bot_node_loc.getX(),
                      bot_node_loc.getY(),
                      bot_l,
                      x,
                      y);
      }

      const int top_l = via_top_layer->getRoutingLevel();
      const Node* node_top
          = Gmat_->getNode(x, y, top_l, top_l == bottom_layer_);
      const Point top_node_loc = node_top->getLoc();
      if (abs(top_node_loc.getX() - x) > node_density_
          || abs(top_node_loc.getY() - y) > node_density_) {
        logger_->warn(utl::PSM,
                      33,
                      "Node at ({}, {}) and layer {} moved from ({}, {}).",
                      top_node_loc.getX(),
                      top_node_loc.getY(),
                      top_l,
                      x,
                      y);
      }
      // Make a connection between the top and bottom nodes of the via
      if (node_bot == nullptr || node_top == nullptr) {
        logger_->error(utl::PSM,
                       34,
                       "Unexpected condition. Null pointer received for node.");
      } else {
        if (R <= 1e-12) {  // if the resistance was not set.
          Gmat_->setConductance(node_bot, node_top, 0);
        } else {
          Gmat_->setConductance(node_bot, node_top, 1 / R);
        }
      }
      // Create the connections in the bottom enclosure
      const auto bot_layer_dir = via_bottom_layer->getDirection();
      if (bot_l != bottom_layer_) {
        const double rho = via_bottom_layer->getResistance();
        if (!checkValidR(rho) && !connection_only) {
          logger_->error(utl::PSM,
                         36,
                         "Layer {} per-unit resistance not found in DB. "
                         "Check the LEF or set it using the command "
                         "'set_layer_rc -layer'.",
                         via_bottom_layer->getName());
        }
        int x_loc1, x_loc2, y_loc1, y_loc2;
        if (bot_layer_dir == dbTechLayerDir::Value::HORIZONTAL) {
          y_loc1 = y - y_cut_size / 2;
          y_loc2 = y + y_cut_size / 2;
          x_loc1 = x - (x_bottom_enclosure + x_cut_size / 2);
          x_loc2 = x + (x_bottom_enclosure + x_cut_size / 2);
        } else {
          y_loc1 = y - (y_bottom_enclosure + y_cut_size / 2);
          y_loc2 = y + (y_bottom_enclosure + y_cut_size / 2);
          x_loc1 = x - x_cut_size / 2;
          x_loc2 = x + x_cut_size / 2;
        }
        Gmat_->generateStripeConductance(via_bottom_layer->getRoutingLevel(),
                                         bot_layer_dir,
                                         x_loc1,
                                         x_loc2,
                                         y_loc1,
                                         y_loc2,
                                         rho);
      }
      // Create the connections in the top enclosure
      const auto top_layer_dir = via_top_layer->getDirection();
      const double rho = via_top_layer->getResistance();
      if (!checkValidR(rho) && !connection_only) {
        logger_->error(utl::PSM,
                       37,
                       "Layer {} per-unit resistance not found in DB. "
                       "Check the LEF or set it using the command "
                       "'set_layer_rc -layer'.",
                       via_top_layer->getName());
      }
      int x_loc1, x_loc2, y_loc1, y_loc2;
      if (top_layer_dir == dbTechLayerDir::Value::HORIZONTAL) {
        y_loc1 = y - y_cut_size / 2;
        y_loc2 = y + y_cut_size / 2;
        x_loc1 = x - (x_top_enclosure + x_cut_size / 2);
        x_loc2 = x + (x_top_enclosure + x_cut_size / 2);
      } else {
        y_loc1 = y - (y_top_enclosure + y_cut_size / 2);
        y_loc2 = y + (y_top_enclosure + y_cut_size / 2);
        x_loc1 = x - x_cut_size / 2;
        x_loc2 = x + x_cut_size / 2;
      }
      Gmat_->generateStripeConductance(via_top_layer->getRoutingLevel(),
                                       top_layer_dir,
                                       x_loc1,
                                       x_loc2,
                                       y_loc1,
                                       y_loc2,
                                       rho);
    } else {
      // If it is a strip we create a connection between all the nodes in the
      // stripe
      dbTechLayer* wire_layer = curWire->getTechLayer();
      int l = wire_layer->getRoutingLevel();
      double rho = wire_layer->getResistance();
      if (!checkValidR(rho) && !connection_only) {
        logger_->error(utl::PSM,
                       66,
                       "Layer {} per-unit resistance not found in DB. "
                       "Check the LEF or set it using the command "
                       "'set_layer_rc -layer'.",
                       wire_layer->getName());
      }
      dbTechLayerDir::Value layer_dir = wire_layer->getDirection();
      if (l == bottom_layer_) {  // ensure that the bottom layer(rail) is
                                 // horizontal
        layer_dir = dbTechLayerDir::Value::HORIZONTAL;
      }
      int x_loc1 = curWire->xMin();
      int x_loc2 = curWire->xMax();
      int y_loc1 = curWire->yMin();
      int y_loc2 = curWire->yMax();
      Gmat_->generateStripeConductance(wire_layer->getRoutingLevel(),
                                       layer_dir,
                                       x_loc1,
                                       x_loc2,
                                       y_loc1,
                                       y_loc2,
                                       rho);
    }
  }
}

//! Function to create the nodes for the c4 bumps
int IRSolver::createC4Nodes(bool connection_only, int unit_micron)
{
  int num_C4 = 0;
  for (size_t it = 0; it < C4Bumps_.size(); ++it) {
    const int x = C4Bumps_[it].x;
    const int y = C4Bumps_[it].y;
    const int size = C4Bumps_[it].size;
    const double v = C4Bumps_[it].voltage;
    const Node* node = Gmat_->getNode(x, y, top_layer_, true);
    const Point node_loc = node->getLoc();
    const double new_loc1 = node_loc.getX() / ((double) unit_micron);
    const double new_loc2 = node_loc.getY() / ((double) unit_micron);
    if (2 * abs(node_loc.getX() - x) > size
        || 2 * abs(node_loc.getY() - y) > size) {
      const double old_loc1 = x / ((double) unit_micron);
      const double old_loc2 = y / ((double) unit_micron);
      const double old_size = size / ((double) unit_micron);
      logger_->warn(utl::PSM,
                    30,
                    "VSRC location at ({:4.3f}um, {:4.3f}um) and "
                    "size {:4.3f}um, is not located on an existing "
                    "power stripe node. Moving to closest node at "
                    "({:4.3f}um, {:4.3f}um).",
                    old_loc1,
                    old_loc2,
                    old_size,
                    new_loc1,
                    new_loc2);
    }
    const NodeIdx k = node->getGLoc();
    const auto ret = C4Nodes_.insert({k, v});
    if (ret.second == false) {
      // key already exists and voltage value is different occurs when a user
      // specifies two different voltage supply values by mistake in two
      // nearby nodes
      logger_->warn(utl::PSM,
                    67,
                    "Multiple voltage supply values mapped"
                    "at the same node ({:4.3f}um, {:4.3f}um)."
                    "If you provided a vsrc file. Check for duplicate entries."
                    "Choosing voltage value {:4.3f}.",
                    new_loc1,
                    new_loc2,
                    ret.first->second);
    } else {
      num_C4++;
    }
  }
  return num_C4;
}

//! Function to find and store the macro boundaries
vector<odb::Rect> IRSolver::getMacroBoundaries()
{
  dbChip* chip = db_->getChip();
  dbBlock* block = chip->getBlock();
  vector<odb::Rect> macro_boundaries;
  for (auto* inst : block->getInsts()) {
    if (inst->isBlock() || inst->isPad()) {
      macro_boundaries.push_back(inst->getBBox()->getBox());
    }
  }
  return macro_boundaries;
}

//! Function to create a G matrix using the nodes
bool IRSolver::createGmat(bool connection_only)
{
  debugPrint(logger_, utl::PSM, "G Matrix", 1, "Creating G matrix");
  dbTech* tech = db_->getTech();
  const int unit_micron = tech->getDbUnitsPerMicron();
  const int num_routing_layers = tech->getRoutingLayerCount();
  dbChip* chip = db_->getChip();
  dbBlock* block = chip->getBlock();

  if (node_density_um_ > 0) {  // User-specified node density
    node_density_ = node_density_um_ * unit_micron;
    logger_->info(
        utl::PSM,
        73,
        "Setting lower metal node density to {}um as specfied by user.",
        node_density_um_);
  } else {  // Node density as a factor of row height either set by user or by
            // default
    dbSet<dbRow> rows = block->getRows();
    const int siteHeight = (*rows.begin())->getSite()->getHeight();
    if (node_density_factor_user_ > 0) {
      node_density_factor_ = node_density_factor_user_;
    }
    logger_->info(utl::PSM,
                  76,
                  "Setting metal node density to "
                  "be standard cell height times {}.",
                  node_density_factor_);
    node_density_ = siteHeight * node_density_factor_;
  }

  Gmat_ = std::make_unique<GMat>(num_routing_layers, logger_);
  const auto macro_boundaries = getMacroBoundaries();
  dbNet* power_net = block->findNet(power_net_.data());
  if (power_net == NULL) {
    logger_->error(utl::PSM,
                   27,
                   "Cannot find net {} in the design. Please provide a valid "
                   "VDD/VSS net.",
                   power_net_);
  }
  power_net_type_ = power_net->getSigType();
  debugPrint(logger_,
             utl::PSM,
             "G Matrix",
             1,
             "Extracting power stripes on net {}",
             power_net->getName());

  // Extract all power wires for the net and store the upper and lower layers
  const vector<dbSBox*> power_wires = findPdnWires(power_net);

  // Create all the nodes for the G matrix
  createGmatViaNodes(power_wires);
  createGmatWireNodes(power_wires, macro_boundaries);

  if (Gmat_->getNumNodes() == 0) {
    logger_->warn(
        utl::PSM, 70, "Net {} has no nodes and will be skipped", power_net_);
    return true;
  }

  // insert c4 bumps as nodes
  const int num_C4 = createC4Nodes(connection_only, unit_micron);

  // All new nodes must be inserted by this point
  // initialize G Matrix
  logger_->info(utl::PSM,
                31,
                "Number of PDN nodes on net {} = {}.",
                power_net_,
                Gmat_->getNumNodes());
  Gmat_->initializeGmatDok(num_C4);

  // Iterate through all the wires to populate conductance matrix
  createGmatConnections(power_wires, connection_only);

  debugPrint(
      logger_, utl::PSM, "G Matrix", 1, "G matrix created successfully.");
  return true;
}

bool IRSolver::checkValidR(double R)
{
  return R >= 1e-12;
}

bool IRSolver::checkConnectivity(bool connection_only)
{
  const CscMatrix* Amat = Gmat_->getAMat();
  const int num_nodes = Gmat_->getNumNodes();
  const int unit_micron = db_->getTech()->getDbUnitsPerMicron();

  queue<Node*> node_q;
  // If we want to test the connectivity of the grid we just start from a single
  // point
  if (connection_only) {
    Node* c4_node = Gmat_->getNode(C4Nodes_.begin()->first);
    node_q.push(c4_node);
  } else {
    // If we do IR analysis, we assume the grid can be connected by different
    // bumps
    for (auto [node_loc, voltage] : C4Nodes_) {
      Node* c4_node = Gmat_->getNode(node_loc);
      node_q.push(c4_node);
    }
  }
  while (!node_q.empty()) {
    Node* node = node_q.front();
    node_q.pop();
    node->setConnected();
    const NodeIdx col_num = node->getGLoc();
    const NodeIdx col_loc = Amat->col_ptr[col_num];
    NodeIdx n_col_loc;
    if (col_num < Amat->col_ptr.size() - 1) {
      n_col_loc = Amat->col_ptr[col_num + 1];
    } else {
      n_col_loc = Amat->row_idx.size();
    }
    const vector<NodeIdx> col_vec(Amat->row_idx.begin() + col_loc,
                                  Amat->row_idx.begin() + n_col_loc);

    for (NodeIdx idx : col_vec) {
      if (idx < num_nodes) {
        Node* node_next = Gmat_->getNode(idx);
        if (!(node_next->getConnected())) {
          node_q.push(node_next);
        }
      }
    }
  }
  int uncon_err_cnt = 0;
  int uncon_inst_cnt = 0;
  bool unconnected_node = false;
  for (Node* node : Gmat_->getAllNodes()) {
    if (!node->getConnected()) {
      uncon_err_cnt++;
      const Point node_loc = node->getLoc();
      const float loc_x = node_loc.getX() / ((float) unit_micron);
      const float loc_y = node_loc.getY() / ((float) unit_micron);
      unconnected_node = true;
      logger_->warn(utl::PSM,
                    38,
                    "Unconnected PDN node on net {} at location ({:4.3f}um, "
                    "{:4.3f}um), layer: {}.",
                    power_net_,
                    loc_x,
                    loc_y,
                    node->getLayerNum());
      if (node->hasInstances()) {
        for (dbInst* inst : node->getInstances()) {
          uncon_inst_cnt++;
          logger_->warn(utl::PSM,
                        39,
                        "Unconnected instance {} at location ({:4.3f}um, "
                        "{:4.3f}um) layer: {}.",
                        inst->getName(),
                        loc_x,
                        loc_y,
                        node->getLayerNum());
        }
      }
    }
  }
  if (unconnected_node == false) {
    logger_->info(
        utl::PSM, 40, "All PDN stripes on net {} are connected.", power_net_);
  }
  return !unconnected_node;
}

bool IRSolver::getConnectionTest()
{
  return connection_;
}

//! Function to get the power value from OpenSTA
/*
 *\return vector of pairs of instance name
 and its corresponding power value
*/
vector<pair<odb::dbInst*, double>> IRSolver::getPower()
{
  debugPrint(
      logger_, utl::PSM, "IR Solver", 1, "Executing STA for power calculation");
  return PowerInst().executePowerPerInst(sta_, logger_);
}

pair<double, double> IRSolver::getSupplyVoltage()
{
  return SupplyVoltage().getSupplyVoltage(sta_, logger_);
}

bool IRSolver::getResult()
{
  return result_;
}

int IRSolver::printSpice()
{
  DokMatrix* Gmat = Gmat_->getGMatDOK();

  ofstream pdnsim_spice_file;
  pdnsim_spice_file.open(spice_out_file_);
  if (!pdnsim_spice_file.is_open()) {
    logger_->error(
        utl::PSM,
        41,
        "Could not open SPICE file {}. Please check if it is a valid path.",
        spice_out_file_);
  }
  const vector<double> J = getJ();
  const int num_nodes = Gmat_->getNumNodes();
  int resistance_number = 0;
  int voltage_number = 0;
  int current_number = 0;

  for (auto it = Gmat->values.begin(); it != Gmat->values.end(); it++) {
    const NodeIdx col = (it->first).first;
    const NodeIdx row = (it->first).second;
    if (col <= row) {
      continue;  // ignore lower half and diagonal as matrix is symmetric
    }
    const double cond = it->second;  // get cond value
    if (abs(cond) < 1e-15) {         // ignore if an empty cell
      continue;
    }

    const string net_name = power_net_;
    if (col < num_nodes) {  // resistances
      const double resistance = -1 / cond;

      const Node* node1 = Gmat_->getNode(col);
      const Node* node2 = Gmat_->getNode(row);
      const Point node_loc1 = node1->getLoc();
      const int x1 = node_loc1.getX();
      const int y1 = node_loc1.getY();
      const int l1 = node1->getLayerNum();
      const string node1_name = net_name + "_" + to_string(x1) + "_"
                                + to_string(y1) + "_" + to_string(l1);

      const Point node_loc2 = node2->getLoc();
      const int x2 = node_loc2.getX();
      const int y2 = node_loc2.getY();
      const int l2 = node2->getLayerNum();
      const string node2_name = net_name + "_" + to_string(x2) + "_"
                                + to_string(y2) + "_" + to_string(l2);

      const string resistance_name = "R" + to_string(resistance_number);
      resistance_number++;

      pdnsim_spice_file << resistance_name << " " << node1_name << " "
                        << node2_name << " " << to_string(resistance) << endl;

      const double current = node1->getCurrent();
      const string current_name = "I" + to_string(current_number);
      if (abs(current) > 1e-18) {
        pdnsim_spice_file << current_name << " " << node1_name << " " << 0
                          << " " << current << endl;
        current_number++;
      }
    } else {                                    // voltage
      const Node* node1 = Gmat_->getNode(row);  // VDD location
      const Point node_loc = node1->getLoc();
      const double voltage_value = J[col];
      const int x1 = node_loc.getX();
      const int y1 = node_loc.getY();
      const int l1 = node1->getLayerNum();
      const string node1_name = net_name + "_" + to_string(x1) + "_"
                                + to_string(y1) + "_" + to_string(l1);
      const string voltage_name = "V" + to_string(voltage_number);
      voltage_number++;
      pdnsim_spice_file << voltage_name << " " << node1_name << " 0 "
                        << to_string(voltage_value) << endl;
    }
  }

  pdnsim_spice_file << ".OPTION NUMDGT=6" << endl;
  pdnsim_spice_file << ".OP" << endl;
  pdnsim_spice_file << ".END" << endl;
  pdnsim_spice_file << endl;
  pdnsim_spice_file.close();
  return 1;
}

int IRSolver::getMinimumResolution()
{
  return node_density_;
}

bool IRSolver::build()
{
  readC4Data();

  bool res = createGmat();
  if (Gmat_->getNumNodes() == 0) {
    connection_ = true;
    return false;
  }

  if (res) {
    res = createJ();
  }
  if (res) {
    res = addC4Bump();
  }
  if (res) {
    res = Gmat_->generateCSCMatrix();
    res = Gmat_->generateACSCMatrix();
  }
  if (res) {
    connection_ = checkConnectivity();
    res = connection_;
  }
  result_ = res;
  return result_;
}

bool IRSolver::buildConnection()
{
  readC4Data();

  bool res = createGmat(true);
  if (Gmat_->getNumNodes() == 0) {
    connection_ = true;
    return true;
  }

  if (res) {
    res = addC4Bump();
  }
  if (res) {
    res = Gmat_->generateACSCMatrix();
  }
  if (res) {
    connection_ = checkConnectivity(true);
    res = connection_;
  }
  result_ = res;
  return result_;
}
}  // namespace psm
