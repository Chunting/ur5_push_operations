#!/usr/bin/env python

from geometry_msgs.msg import Pose, Quaternion
from pyquaternion import Quaternion as PyQuaternion
import numpy as np

class Push(object):
    __slots__ = ['id', 'approach', 'angle', 'distance']

def get_diff_pose(p1, p2):
    t1 = pose_to_matrix(p1)
    t2 = pose_to_matrix(p2)
    t3 = np.dot(np.linalg.inv(t1), t2)
    return matrix_to_pose(t3)

def transform_pose(base, pose):
    tb = pose_to_matrix(base)
    tp = pose_to_matrix(pose)
    return matrix_to_pose(np.dot(tb, tp))

def interpolate_poses(p1, p2):
    p3 = Pose()
    p3.position.x = 0.5 * (p1.position.x + p2.position.x)
    p3.position.y = 0.5 * (p1.position.y + p2.position.y)
    p3.position.z = 0.5 * (p1.position.z + p2.position.z)
    p3.orientation = quat_slerp(p1.orientation, p2.orientation)
    return p3


def get_diff_pose_old(p1, p2):
    pose = Pose()
    pose.position.x = p2.position.x - p1.position.x
    pose.position.y = p2.position.y - p1.position.y
    pose.position.z = p2.position.z - p1.position.z
    pose.orientation = quat_difference(p1.orientation, p2.orientation)
    return pose

def pose_to_matrix(pose):
    T = quat_geom_to_py(pose.orientation).transformation_matrix
    T[0][-1] = pose.position.x
    T[1][-1] = pose.position.y
    T[2][-1] = pose.position.z
    return T

def matrix_to_pose(mat):
    pose = Pose()
    pose.position.x = mat[0][-1]
    pose.position.y = mat[1][-1]
    pose.position.z = mat[2][-1]
    pose.orientation = quat_py_to_geom(PyQuaternion(matrix=mat))
    return pose

def quat_slerp(q1, q2):
    if isinstance(q1, PyQuaternion) and isinstance(q2, PyQuaternion):
        return PyQuaternion.slerp(q1, q2)
    elif isinstance(q1, Quaternion) and isinstance(q2, Quaternion):
        q3 = PyQuaternion.slerp(quat_geom_to_py(q1), quat_geom_to_py(q2))
        return quat_py_to_geom(q3)
    else:
        return None

def quat_geom_to_py(geomquat):
    return PyQuaternion(w=geomquat.w, x=geomquat.x, y=geomquat.y, z=geomquat.z)

def quat_py_to_geom(pyquat):
    q = Quaternion()
    q.w = pyquat.elements[0]
    q.x = pyquat.elements[1]    
    q.y = pyquat.elements[2]
    q.z = pyquat.elements[3]
    return q

def quat_difference(q1, q2):
    if isinstance(q1, PyQuaternion) and isinstance(q2, PyQuaternion):
        return q1 / q2
    elif isinstance(q1, Quaternion) and isinstance(q2, Quaternion):
        q3 = quat_geom_to_py(q1) / quat_geom_to_py(q2)
        return quat_py_to_geom(q3)

def quat_from_yaw(yaw):
    return quat_py_to_geom(PyQuaternion(axis=[0,0,1], angle=yaw))

def get_yaw(pose):
    q = quat_geom_to_py(pose.orientation)
    return q.angle * q.axis[2]

def get_line(p1, p2):
    A = (p1.y - p2.y)
    B = (p2.x - p1.x)
    C = (p1.x*p2.y - p2.x*p1.y)
    return A, B, -C

def intersection(L1, L2):
    D  = L1[0] * L2[1] - L1[1] * L2[0]
    Dx = L1[2] * L2[1] - L1[1] * L2[2]
    Dy = L1[0] * L2[2] - L1[2] * L2[0]
    if D != 0:
        x = Dx / D
        y = Dy / D
        return x,y
    else:
        return False
