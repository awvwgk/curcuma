/*
 * <Geometry tools for chemical structures.>
 * Copyright (C) 2019 - 2020 Conrad Hübler <Conrad.Huebler@gmx.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#pragma once

#include <Eigen/Dense>

#include "src/core/global.h"
#include "src/core/molecule.h"

namespace GeometryTools {



inline Geometry TranslateMolecule(const Molecule &molecule, const Position &start, const Position &destination)
{
    Geometry geom = molecule.getGeometry();

    Position direction = destination - start;

    for(int i = 0; i < geom.rows(); ++i)
        geom.row(i) += direction;

    return geom;

}

inline double Distance(const Position& a, const Position& b)
{
    double distance = 0;
    distance = sqrt((a(0) - b(0)) * (a(0) - b(0)) + (a(1) - b(1)) * (a(1) - b(1)) + (a(2) - b(2)) * (a(2) - b(2)));
    return distance;
}
}
