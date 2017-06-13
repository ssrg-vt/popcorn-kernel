/**
 * @file vma_server.h
 * (public interface)
 *
 * Popcorn Linux VMA server public interface
 * This work is an extension of David Katz MS Thesis, please refer to the
 * Thesis for further information about the algorithm.
 *
 * @author Antonio Barbalace, SSRG Virginia Tech 2016
 */

#ifndef INCLUDE_POPCORN_VMA_SERVER_H_
#define INCLUDE_POPCORN_VMA_SERVER_H_

int vma_server_fetch_vma(struct task_struct *tsk, unsigned long address);

#endif /* INCLUDE_POPCORN_VMA_SERVER_H_ */
