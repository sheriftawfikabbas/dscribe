/*Copyright 2019 DScribe developers

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include <iostream>
#include <unordered_map>
//#include <unordered_set>
#include "descriptor.h"

using namespace std;

Descriptor::Descriptor(string average, double cutoff)
    : average(average)
    , cutoff(cutoff)
{
}

void Descriptor::derivatives_numerical(
    py::array_t<double> out_d, 
    py::array_t<double> out, 
    py::array_t<double> positions,
    py::array_t<int> atomic_numbers,
    py::array_t<double> centers,
    py::array_t<int> center_indices,
    py::array_t<int> indices,
    bool return_descriptor
) const
{
    // The general idea: each atom for which a derivative is requested is
    // "wiggled" with central finite difference. The following tricks are used
    // to speed up the calculation:
    //  - The CellList for positions is calculated only once and passed to the
    //    create-method.
    //  - Only centers within the cutoff distance from the wiggled atom are
    //    taken into account by calculating a separate CellList for the centers.
    //  - Atoms for which there are no neighouring centers are skipped
    //  - Self-interaction is ignored (the derivative of atom with respect to
    //    itself is always zero).
    //  TODO:
    //  - Symmetry of the derivatives is taken into account (derivatives of
    //    [i, j] is -[j, i] AND species position should be swapped)
    //  - Using symmetry and removing self-intearciont in the averaged case is
    //    much more difficult, especially in the inner-averaging mode. Thus these
    //    optimization are simply left out.
    int n_features = this->get_number_of_features();
    auto out_d_mu = out_d.mutable_unchecked<4>();
    auto indices_u = indices.unchecked<1>();
    auto positions_mu = positions.mutable_unchecked<2>();
    auto centers_u = centers.unchecked<2>();
    auto center_indices_u = center_indices.unchecked<1>();
    int n_all_centers = center_indices.size();

    // Calculate neighbours with a cell list.
    CellList cell_list_atoms(positions, this->cutoff);
    CellList cell_list_centers(centers, this->cutoff);

    // TODO: These are needed for tracking symmetrical values.
    // Create mappings between center index and atom index and vice versa. The
    // order of centers and indices can be arbitrary, and not all centers
    // correspond to atoms.
    unordered_map<int, int> index_atom_map;
    unordered_map<int, int> center_atom_map;
    //unordered_map<int, int> index_center_map;
    //unordered_map<int, int> atom_center_map;
    //for (int i=0; i < center_indices.size(); ++i) {
        //int index = center_indices_u(i);
        //if (index != -1) {
            //index_center_map[index] = i;
        //}
    //}
    //for (int i=0; i < indices.size(); ++i) {
        //int index = indices_u(i);
        //if (index_center_map.find(index) != index_center_map.end()) {
            //atom_center_map[i] = index_center_map[index];
        //}
    //}
    for (int i=0; i < indices.size(); ++i) {
        int index = indices_u(i);
        index_atom_map[index] = i;
    }
    for (int i=0; i < center_indices.size(); ++i) {
        int index = center_indices_u(i);
        if (index != -1 && index_atom_map.find(index) != index_atom_map.end()) {
            center_atom_map[i] = index_atom_map[index];
        }
    }

    // Calculate the desciptor value if requested
    if (return_descriptor) {
        this->create(out, positions, atomic_numbers, centers, cell_list_atoms);
    }
    
    // Central finite difference with error O(h^2)
    double h = 0.0001;
    vector<double> coefficients = {-1.0/2.0, 1.0/2.0};
    vector<double> displacement = {-1.0, 1.0};

    // Loop over all atoms
    for (int i_idx=0; i_idx < indices_u.size(); ++i_idx) {
        int i_atom = indices_u(i_idx);

        // Check whether the atom has any centers within radius. If not, the
        // calculation is skipped.
        double ix = positions_mu(i_atom, 0);
        double iy = positions_mu(i_atom, 1);
        double iz = positions_mu(i_atom, 2);
        vector<int> centers_local_idx = cell_list_centers.getNeighboursForPosition(ix, iy, iz).indices;
        int n_locals = centers_local_idx.size();
        if (n_locals == 0) {
            continue;
        }

        // When averaging is not performed, only use the local centers and
        // remove self-interaction, as these will simply be zeroes.
        // TODO Remove half of symmetrical pairs
        int n_centers;
        py::array_t<double> centers_local_pos;
        if (this->average == "off") {
            //bool not_center = atom_center_map.find(i_atom) == atom_center_map.end();
            //unordered_set<int> symmetric;
            vector<int> locals;
            for (int i = 0; i < centers_local_idx.size(); ++i) {
                int local_idx = centers_local_idx[i];
                auto center_atom_idx = center_atom_map.find(local_idx);
                if (center_atom_idx == center_atom_map.end()) {
                    locals.push_back(local_idx);
                } else if (center_atom_idx->second != i_atom) {
                    locals.push_back(local_idx);
                }
                //if (not_center || center_atom_idx == center_atom_map.end()) {
                    //locals.push_back(local_idx);
                //} else if (center_atom_idx->second > i_atom) {
                    //locals.push_back(local_idx);
                    //symmetric.insert(local_idx);
                //}
            }
            centers_local_idx = locals;
            n_locals = centers_local_idx.size();
            if (n_locals == 0) {
                continue;
            }

            // Create a new list containing only the nearby centers, taking into
            // account symmetry of the derivatives and zero self-interaction.
            centers_local_pos = py::array_t<double>({n_locals, 3});
            auto centers_local_pos_mu = centers_local_pos.mutable_unchecked<2>();
            for (int i_local = 0; i_local < n_locals; ++i_local) {
                int i_local_idx = centers_local_idx[i_local];
                for (int i_comp = 0; i_comp < 3; ++i_comp) {
                    centers_local_pos_mu(i_local, i_comp) = centers_u(i_local_idx, i_comp);
                }
            }
            n_centers = n_locals;
        } else {
            centers_local_pos = centers;
            centers_local_idx = vector<int>{0};
            n_centers = 1;
        }

        // Create a copy of the original atom position.
        py::array_t<double> pos(3);
        auto pos_mu = pos.mutable_unchecked<1>();
        for (int i = 0; i < 3; ++i) {
            pos_mu(i) = positions_mu(i_atom, i);
        }

        for (int i_comp=0; i_comp < 3; ++i_comp) {
            for (int i_stencil=0; i_stencil < 2; ++i_stencil) {

                // Introduce the displacement
                positions_mu(i_atom, i_comp) = pos_mu(i_comp) + h*displacement[i_stencil];

                // Initialize temporary numpy array for storing the descriptor
                // for this stencil point
                double* dTemp = new double[n_centers*n_features]();
                py::array_t<double> d({n_centers, n_features}, dTemp);

                // Calculate descriptor value
                this->create(d, positions, atomic_numbers, centers_local_pos, cell_list_atoms);
                auto d_u = d.unchecked<2>();

                // Add value to final derivative array
                double coeff = coefficients[i_stencil];
                for (int i_local=0; i_local < n_centers; ++i_local) {
                    int i_center = centers_local_idx[i_local];
                    for (int i_feature=0; i_feature < n_features; ++i_feature) {
                        double value = coeff*d_u(i_local, i_feature);
                        out_d_mu(i_center, i_idx, i_comp, i_feature) = out_d_mu(i_center, i_idx, i_comp, i_feature) + value;
                        //if (symmetric.find(i_center) != symmetric.end()) {
                            //out_d_mu(atom_center_map[i_idx], center_atom_map[i_center], i_comp, i_feature) = out_d_mu(atom_center_map[i_idx], center_atom_map[i_center], i_comp, i_feature) - value;
                        //}
                    }
                }

                delete [] dTemp;
            }
            for (int i_local=0; i_local < n_centers; ++i_local) {
                int i_center = centers_local_idx[i_local];
                for (int i_feature=0; i_feature < n_features; ++i_feature) {
                    out_d_mu(i_center, i_idx, i_comp, i_feature) = out_d_mu(i_center, i_idx, i_comp, i_feature) / h;
                    //if (symmetric.find(i_center) != symmetric.end()) {
                        //out_d_mu(atom_center_map[i_idx], center_atom_map[i_center], i_comp, i_feature) = out_d_mu(atom_center_map[i_idx], center_atom_map[i_center], i_comp, i_feature) / h;
                    //}
                }
            }

            // Return position back to original value for next component
            positions_mu(i_atom, i_comp) = pos_mu(i_comp);
        }
    }
}
